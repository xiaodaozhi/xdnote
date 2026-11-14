# All inputs and registry mutations belong to an empty disposable CI account.
# QA .exe payload files are intentionally invalid and are NEVER executed.
param([string]$FixtureDirectory = 'build/installer-smoke')
$ErrorActionPreference = 'Stop'
if ($env:CI -ne 'true' -or $env:RUNNER_OS -ne 'Windows') { throw 'Disposable Windows CI only' }
. "$PSScriptRoot/windows-installer-paths.ps1"
$root = Join-Path $env:RUNNER_TEMP 'xdnote 安装器 smoke, space'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'xdnote.lnk'
foreach ($key in @('Software\Classes\xdnote.Markdown', 'Software\Classes\Applications\xdnote.exe', 'Software\xdnote')) {
    if ([Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($key)) { throw 'Refusing to overwrite an existing application registration' }
}
if ((Test-Path -LiteralPath $root) -or (Test-Path -LiteralPath $shortcut)) { throw 'Refusing to overwrite existing files' }
$uninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{460D7C68-C989-45F0-B312-40AE18428155}_is1'
if ([Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($uninstallKey)) { throw 'Existing installer registration' }
$data = Join-Path $env:RUNNER_TEMP 'xdnote installer user data'
New-Item -ItemType Directory $data | Out-Null
$sentinel = Join-Path $data 'settings-and-draft.md'
Set-Content -LiteralPath $sentinel -Value 'settings recovery documents must survive' -Encoding utf8
$sentinelHash = (Get-FileHash -LiteralPath $sentinel).Hash
$completed = [Collections.Generic.List[string]]::new()
function Invoke-Package([string]$Package, [bool]$Success=$true, [string]$Language='english', [string]$Tasks='markdown') {
    $log = Join-Path $FixtureDirectory ('native-' + [Guid]::NewGuid().ToString('N') + '.log')
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="'+$root+'"'),('/LANG='+$Language),('/TASKS='+$Tasks),('/LOG="'+[IO.Path]::GetFullPath($log)+'"'))
    $process = Start-Process -FilePath $Package -WindowStyle Hidden -ArgumentList $arguments -Wait -PassThru
    if ($Success -and $process.ExitCode -ne 0) { throw "Expected success, got $($process.ExitCode); inspect $log" }
    if (!$Success -and $process.ExitCode -eq 0) { throw "Expected protection refusal; inspect $log" }
}
function Read-Registry([string]$Key, [string]$Name='') {
    $handle = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Key)
    if (!$handle) { return $null }
    try { return $handle.GetValue($Name, $null) } finally { $handle.Dispose() }
}
foreach ($ext in @('md','markdown')) {
    $handle = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("Software\Classes\.$ext\OpenWithProgids")
    $handle.SetValue('xdnote.QAForeign', [byte[]]@(), [Microsoft.Win32.RegistryValueKind]::None); $handle.Dispose()
}
$defaults = @{}
foreach ($ext in @('md','markdown')) {
    $defaults[$ext] = Read-Registry "Software\Classes\.$ext"
    $defaults[$ext+'UserChoice'] = Read-Registry "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$ext\UserChoice" 'ProgId'
}
try {
    # AppMutex rejects maintenance while a potentially dirty application exists.
    $running = [Threading.Mutex]::new($false, 'Local\xdnote.desktop.running')
    try { Invoke-Package (Join-Path $FixtureDirectory 'qa1.exe') $false } finally { $running.Dispose() }
    if (Test-Path -LiteralPath (Join-Path $root 'xdnote.exe')) { throw 'Running-app protection copied payload files' }
    $completed.Add('running-app protection')
    Invoke-Package (Join-Path $FixtureDirectory 'qa1.exe')
    Assert-XdnoteShortcut (Join-Path $root 'xdnote.exe')
    $command = '"' + (Join-Path $root 'xdnote.exe') + '" -- "%1"'
    if ((Read-Registry 'Software\Classes\xdnote.Markdown\shell\open\command') -ne $command) { throw 'Incorrect Unicode open command' }
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes\xdnote.Markdown')
    try { if ($key.GetValueKind('AllowSilentDefaultTakeOver') -ne [Microsoft.Win32.RegistryValueKind]::None) { throw 'Missing REG_NONE protection' } } finally { $key.Dispose() }
    $completed.Add('fresh install and Unicode shortcut')
    $keep = Join-Path $root 'retired/user-kept.md'; Set-Content -LiteralPath $keep -Value 'keep unknown user file'
    $old = Join-Path $root 'retired/old-owned.txt'
    $oldBytes = [IO.File]::ReadAllBytes($old); [IO.File]::WriteAllText($old, 'modified user content')
    Invoke-Package (Join-Path $FixtureDirectory 'qa2.exe') $false
    if ([IO.File]::ReadAllText($old) -ne 'modified user content') { throw 'Modified obsolete file was overwritten' }
    [IO.File]::WriteAllBytes($old, $oldBytes)
    $completed.Add('modified obsolete file refusal')
    $exe = Join-Path $root 'xdnote.exe'; $exeBytes = [IO.File]::ReadAllBytes($exe)
    [IO.File]::WriteAllText($exe, 'modified still-current file')
    Invoke-Package (Join-Path $FixtureDirectory 'qa2.exe') $false
    if ([IO.File]::ReadAllText($exe) -ne 'modified still-current file') { throw 'Modified active file was overwritten' }
    [IO.File]::WriteAllBytes($exe, $exeBytes)
    $completed.Add('modified active file refusal')
    $manifest = Join-Path $root 'install-manifest.txt'; $manifestBytes = [IO.File]::ReadAllBytes($manifest)
    [IO.File]::WriteAllText($manifest, "xdnote-install-files-v1`n" + ('0'*64) + "`t..\escape.md`n")
    Invoke-Package (Join-Path $FixtureDirectory 'qa2.exe') $false
    [IO.File]::WriteAllBytes($manifest, $manifestBytes)
    $completed.Add('invalid manifest refusal')
    $handle = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\xdnote\Install')
    $handle.SetValue('SchemaVersion', 2, [Microsoft.Win32.RegistryValueKind]::DWord); $handle.Dispose()
    Invoke-Package (Join-Path $FixtureDirectory 'qa2.exe') $false
    $handle = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\xdnote\Install')
    $handle.SetValue('SchemaVersion', 1, [Microsoft.Win32.RegistryValueKind]::DWord); $handle.Dispose()
    $completed.Add('newer installer schema refusal')
    Invoke-Package (Join-Path $FixtureDirectory 'qa2.exe') $true 'chinesesimplified'
    Assert-XdnoteShortcut $exe
    if ((Test-Path -LiteralPath $old) -or !(Test-Path -LiteralPath $keep)) { throw 'Upgrade cleanup ownership failure' }
    $expected = (Get-Content (Join-Path $FixtureDirectory 'qa2.json') -Raw | ConvertFrom-Json).payloadManifestSha256
    if ((Get-FileHash -LiteralPath $manifest).Hash.ToLowerInvariant() -ne $expected) { throw 'Installed manifest differs' }
    $completed.Add('upgrade and obsolete cleanup')
    # Recreated paths which belonged to an old version are now unowned.
    [IO.File]::WriteAllText($old, 'new user data at an obsolete path')
    foreach ($ext in @('md','markdown')) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Classes\.$ext\OpenWithProgids")
        try { if ($key.GetValueNames() -notcontains 'xdnote.QAForeign' -or $key.GetValueNames() -notcontains 'xdnote.Markdown') { throw 'Candidate registration lost ownership' } } finally { $key.Dispose() }
        if ((Read-Registry "Software\Classes\.$ext") -ne $defaults[$ext] -or (Read-Registry "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$ext\UserChoice" 'ProgId') -ne $defaults[$ext+'UserChoice']) { throw 'Default application was changed' }
    }
    $completed.Add('default and foreign candidates preserved')
    $exeBytes = [IO.File]::ReadAllBytes($exe)
    [IO.File]::WriteAllText($exe, 'preserve modified file during uninstall')
    Invoke-Package (Join-Path $root 'unins000.exe') $false
    if ([IO.File]::ReadAllText($exe) -ne 'preserve modified file during uninstall') { throw 'Uninstall touched modified file' }
    [IO.File]::WriteAllBytes($exe, $exeBytes)
    $completed.Add('modified uninstall refusal')
    Invoke-Package (Join-Path $root 'unins000.exe')
    if ((Test-Path -LiteralPath $exe) -or (Test-Path -LiteralPath $manifest) -or (Test-Path -LiteralPath $shortcut)) { throw 'Owned uninstall failed' }
    if (!(Test-Path -LiteralPath $keep) -or [IO.File]::ReadAllText($old) -ne 'new user data at an obsolete path') { throw 'Uninstall removed unowned files' }
    if ($null -ne (Read-Registry 'Software\Classes\xdnote.Markdown\shell\open\command')) { throw 'Owned registration was retained' }
    $completed.Add('uninstall preserves extra and recreated files')
    # Fresh installs reject a nonempty destination and cannot claim it.
    Invoke-Package (Join-Path $FixtureDirectory 'qa1.exe') $false
    $completed.Add('nonempty unowned destination refusal')
    $root = Join-Path $env:RUNNER_TEMP 'xdnote no association smoke'
    if (Test-Path -LiteralPath $root) { throw 'Fresh destination already exists' }
    Invoke-Package (Join-Path $FixtureDirectory 'qa2.exe') $true 'english' ''
    if ($null -ne (Read-Registry 'Software\Classes\xdnote.Markdown\shell\open\command')) { throw 'Unchecked registration task wrote candidates' }
    Invoke-Package (Join-Path $root 'unins000.exe')
    $completed.Add('optional association task stays unchecked')
    # A later copy may take over the same ProgID; uninstall must preserve it.
    $root = Join-Path $env:RUNNER_TEMP 'xdnote foreign owner smoke'
    if (Test-Path -LiteralPath $root) { throw 'Foreign-owner destination already exists' }
    Invoke-Package (Join-Path $FixtureDirectory 'qa2.exe')
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Classes\xdnote.Markdown\shell\open\command')
    $key.SetValue('', '"C:\Other Copy\xdnote.exe" -- "%1"'); $key.Dispose()
    Invoke-Package (Join-Path $root 'unins000.exe')
    if ((Read-Registry 'Software\Classes\xdnote.Markdown\shell\open\command') -ne '"C:\Other Copy\xdnote.exe" -- "%1"') { throw 'Uninstall deleted another copy registration' }
    $completed.Add('retargeted registration preserved')
    if ((Get-FileHash -LiteralPath $sentinel).Hash -ne $sentinelHash) { throw 'User data sentinel was changed' }
    $completed.Add('user data sentinel preserved')
    @{ok=$true; scope='disposable Windows CI, native installer with file fixtures; no product launch'; checks=$completed} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $FixtureDirectory 'native-report.json') -Encoding utf8
} finally {
    # Never recursively delete the install root: keep failed evidence and sentinels.
    foreach ($ext in @('md','markdown')) {
        $handle = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Classes\.$ext\OpenWithProgids", $true)
        if ($handle) { try { $handle.DeleteValue('xdnote.QAForeign', $false) } finally { $handle.Dispose() } }
    }
}
