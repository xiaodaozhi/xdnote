# Actual installer and payload integration; never run against a personal account.
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$Audit,
    [Parameter(Mandatory=$true)][string]$PackageReport,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$QtPrefix,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
if ($env:CI -ne 'true' -or $env:RUNNER_OS -ne 'Windows' -or $env:GITHUB_RUN_ID -notmatch '^\d+$') { throw 'Disposable Windows CI only' }
. "$PSScriptRoot/windows-installer-paths.ps1"
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build')) + [IO.Path]::DirectorySeparatorChar
$output = [IO.Path]::GetFullPath($EvidenceDirectory)
if (!$output.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)) { throw 'Use a new evidence directory under this project build/' }
$root = Join-Path $env:RUNNER_TEMP ('xdnote 产品安装, ' + $env:GITHUB_RUN_ID)
$data = Join-Path $env:RUNNER_TEMP ('xdnote 保留数据, ' + $env:GITHUB_RUN_ID)
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'xdnote.lnk'
if ((Test-Path -LiteralPath $root) -or (Test-Path -LiteralPath $data) -or (Test-Path -LiteralPath $shortcut)) { throw 'Existing installation or test data; preserved' }
foreach ($key in @('Software\Classes\xdnote.Markdown', 'Software\Classes\Applications\xdnote.exe', 'Software\xdnote', 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{460D7C68-C989-45F0-B312-40AE18428155}_is1')) {
    $handle = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($key)
    if ($handle) { $handle.Dispose(); throw 'Existing application registration; preserved' }
}
$metadata = Get-Content -LiteralPath $Audit -Raw | ConvertFrom-Json
$packageIdentity = Get-Content -LiteralPath $PackageReport -Raw | ConvertFrom-Json
if (!$metadata.ok -or $packageIdentity.qaFixture -or $packageIdentity.dirty -or !$packageIdentity.ok) { throw 'Clean actual product package required' }
if ((Get-FileHash -LiteralPath $Package).Hash.ToLowerInvariant() -ne $packageIdentity.sha256) { throw 'Installer no longer matches its report' }
New-Item -ItemType Directory $output | Out-Null
New-Item -ItemType Directory $data | Out-Null
$sentinel = Join-Path $data '配置与恢复样本.md'
Set-Content -LiteralPath $sentinel -Value 'own fixture configuration and recovery data' -Encoding utf8
$sentinelHash = (Get-FileHash -LiteralPath $sentinel).Hash
$checks = [Collections.Generic.List[string]]::new()
$ok = $false
$defaultsChanged = $null
$registrationObserved = $null
function Read-Value([string]$Key, [string]$Name='') {
    $handle = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Key)
    if (!$handle) { return $null }
    try { return $handle.GetValue($Name, $null) } finally { $handle.Dispose() }
}
$defaults = @{}
foreach ($ext in @('md','markdown')) {
    $defaults[$ext] = Read-Value "Software\Classes\.$ext"
    $defaults[$ext+'UserChoice'] = Read-Value "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$ext\UserChoice" 'ProgId'
}
function Invoke-Maintenance([string]$Executable, [string]$Name, [bool]$Success=$true) {
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/LANG=chinesesimplified','/TASKS=markdown',('/DIR="'+$root+'"'),('/LOG="'+(Join-Path $output ($Name+'.log'))+'"'))
    $process = Start-Process -FilePath $Executable -WindowStyle Hidden -ArgumentList $arguments -Wait -PassThru
    if ($Success -and $process.ExitCode -ne 0) { throw "$Name failed: $($process.ExitCode)" }
    if (!$Success -and $process.ExitCode -eq 0) { throw "$Name did not protect modified files" }
}
function Assert-Payload {
    foreach ($item in $metadata.files) {
        $file = Join-Path $root $item.path
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne $item.bytes -or (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $item.sha256) { throw "Installed payload differs: $($item.path)" }
    }
    if ((Get-FileHash -LiteralPath (Join-Path $root 'install-manifest.txt')).Hash.ToLowerInvariant() -ne $packageIdentity.payloadManifestSha256) { throw 'Installed manifest differs' }
    Assert-XdnoteShortcut (Join-Path $root 'xdnote.exe') $output
}
function Invoke-InstalledProbe([string]$Name) {
    # The diagnostic stays outside the installed product. Its native windows
    # are hidden and it loads DLL/plugins exclusively from the installed root.
    & python "$PSScriptRoot/app_performance.py" --build-dir $BuildDirectory --qt-prefix $QtPrefix --payload $root --scenario split --repeats 1 --output (Join-Path $output $Name)
    if ($LASTEXITCODE -ne 0) { throw "Installed runtime probe failed: $Name" }
    $report = Get-Content (Join-Path $output "$Name/report.json") -Raw | ConvertFrom-Json
    if (!$report.scenarios.split.raw[0].editUndoSaveIntegrity) { throw 'Installed edit/undo/save path failed' }
}
try {
    Invoke-Maintenance $Package 'install'
    Assert-Payload
    $checks.Add('actual payload install, manifest and Unicode shortcut')
    $command = '"' + (Join-Path $root 'xdnote.exe') + '" -- "%1"'
    if ((Read-Value 'Software\Classes\xdnote.Markdown\shell\open\command') -ne $command) { throw 'Installed association command is incorrect' }
    $registrationObserved = $true
    $checks.Add('actual installed executable candidate registration')
    Invoke-InstalledProbe 'installed-runtime'
    $checks.Add('installed native runtime, edit, undo, modes and saved bytes')
    $core = Join-Path $root 'Qt6Core.dll'; $original = [IO.File]::ReadAllBytes($core)
    try {
        # Never execute a modified library. Check maintenance refusal, restore
        # the exact original bytes, and only then run application probes again.
        [IO.File]::AppendAllText($core, 'qa material guard')
        $modified = (Get-FileHash -LiteralPath $core).Hash
        Invoke-Maintenance $Package 'modified-upgrade-refusal' $false
        Invoke-Maintenance (Join-Path $root 'unins000.exe') 'modified-uninstall-refusal' $false
        if ((Get-FileHash -LiteralPath $core).Hash -ne $modified) { throw 'Maintenance overwrote modified Qt library' }
    } finally { [IO.File]::WriteAllBytes($core, $original) }
    $checks.Add('actual Qt library modification blocks upgrade and uninstall')
    $keep = Join-Path $root '用户保留文档.md'; Set-Content -LiteralPath $keep -Value 'preserve unknown user document' -Encoding utf8
    $keepHash = (Get-FileHash -LiteralPath $keep).Hash
    Invoke-Maintenance $Package 'upgrade'
    Assert-Payload
    Invoke-InstalledProbe 'upgraded-runtime'
    if ((Get-FileHash -LiteralPath $keep).Hash -ne $keepHash) { throw 'Upgrade changed unknown user file' }
    $checks.Add('actual upgrade and upgraded runtime preserve user fixture')
    Invoke-Maintenance (Join-Path $root 'unins000.exe') 'uninstall'
    foreach ($item in $metadata.files) { if (Test-Path -LiteralPath (Join-Path $root $item.path)) { throw "Uninstall retained owned payload: $($item.path)" } }
    if ((Test-Path -LiteralPath $shortcut) -or (Test-Path -LiteralPath (Join-Path $root 'install-manifest.txt'))) { throw 'Owned shortcut or manifest retained' }
    if ($null -ne (Read-Value 'Software\Classes\xdnote.Markdown\shell\open\command')) { throw 'Owned association was retained' }
    if ((Get-FileHash -LiteralPath $keep).Hash -ne $keepHash -or (Get-FileHash -LiteralPath $sentinel).Hash -ne $sentinelHash) { throw 'Uninstall changed user fixtures' }
    $checks.Add('actual uninstall removes owned files and preserves user fixtures')
    $defaultsChanged = $false
    foreach ($ext in @('md','markdown')) {
        if ((Read-Value "Software\Classes\.$ext") -ne $defaults[$ext] -or (Read-Value "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$ext\UserChoice" 'ProgId') -ne $defaults[$ext+'UserChoice']) { $defaultsChanged = $true; throw 'Default application changed' }
    }
    $checks.Add('default applications unchanged through actual package roundtrip')
    $ok = $true
} finally {
    @{ok=$ok; checks=$checks; buildIdentity=$metadata.buildIdentity; installerSha256=$packageIdentity.sha256; installedFiles=$metadata.files.Count; realRegistryChanged=$registrationObserved; defaultsChanged=$defaultsChanged; scope='actual native installer and installed runtime through hidden-native application services probe; no Explorer/default-app/visible-wizard acceptance'} | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output 'report.json') -Encoding utf8
    # Failed installations and self-created data are retained for diagnosis.
    # No recursive deletion of the installation or any user profile directory.
}
