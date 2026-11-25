$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'Run the disposable account check on a GitHub-hosted Windows runner.'
}
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The disposable runner account must be able to create a standard test account.'
}

$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$base = (Resolve-Path -LiteralPath (Join-Path $repo 'build/cycle-268-appcontainer')).Path
$id = [guid]::NewGuid().ToString('N')
$runtime = Join-Path $base "cross-account-$id"
$profileName = "xdnote.ipc.account.$id"
$accountName = 'xdnqa' + $id.Substring(0,12)
$python = (Get-Command python.exe -CommandType Application).Source
$account = $server = $client = $null
New-Item -ItemType Directory -Path $runtime | Out-Null

try {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'cross_account.py'),(Join-Path $PSScriptRoot 'cross_container.py') -Destination $runtime
    foreach ($name in 'product_instance_probe.exe','container_isolation_probe.exe') {
        Copy-Item -LiteralPath (Join-Path $base "out/$name") -Destination $runtime
    }
    foreach ($name in 'Qt6Core.dll','Qt6Network.dll') {
        Copy-Item -LiteralPath (Join-Path $repo "build/Qt/6.8.2/msvc2022_64/bin/$name") -Destination $runtime
    }
    foreach ($name in 'msvcp140.dll','msvcp140_1.dll','vcruntime140.dll','vcruntime140_1.dll') {
        Copy-Item -LiteralPath (Join-Path $repo "build/toolchain/windows-runtime/runtime/$name") -Destination $runtime
    }

    $password = ConvertTo-SecureString ('Ad1!' + [guid]::NewGuid().ToString('N')) -AsPlainText -Force
    $account = New-LocalUser -Name $accountName -Password $password -Description 'Temporary xdnote IPC test account'
    $users = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-545')
    if (!(Get-LocalGroupMember -SID $users | Where-Object SID -eq $account.SID)) {
        Add-LocalGroupMember -SID $users -Member $account
    }
    & icacls.exe $runtime '/grant' ('*' + $account.SID.Value + ':(OI)(CI)(RX)') | Out-Null
    if ($LASTEXITCODE) { throw 'Could not grant the disposable account runtime read access.' }
    $credential = [pscredential]::new("$env:COMPUTERNAME\$accountName", $password)
    $script = Join-Path $runtime 'cross_account.py'
    $client = Start-Process -FilePath $python -ArgumentList @('-B', ('"' + $script + '"'), 'query', ('"' + $runtime + '"'), $profileName) -WorkingDirectory $runtime -Credential $credential -LoadUserProfile -WindowStyle Hidden -PassThru
    $server = Start-Process -FilePath $python -ArgumentList @('-B', ('"' + $script + '"'), 'serve', ('"' + $runtime + '"'), $profileName) -WorkingDirectory $runtime -WindowStyle Hidden -RedirectStandardError (Join-Path $runtime 'server-error.log') -PassThru
    if (!$client.WaitForExit(40000)) { throw 'The second account did not complete the check.' }
    if ($client.ExitCode -notin 12,15) {
        if ($server.HasExited) { Get-Content -LiteralPath (Join-Path $runtime 'server-error.log') -Tail 10 }
        throw "Cross-account access check produced unexpected exit code $($client.ExitCode)."
    }
    New-Item -ItemType File -Path (Join-Path $runtime 'stop') | Out-Null
    if (!$server.WaitForExit(15000) -or $server.ExitCode) {
        Get-Content -LiteralPath (Join-Path $runtime 'server-error.log') -Tail 10
        throw 'The product listener did not finish the same-account control.'
    }
    $result = [ordered]@{differentUsers=$true; sameAppContainerIdentity=$true; owningAccountAccess=$true; otherAccountError=($client.ExitCode - 10)}
} finally {
    $stop = Join-Path $runtime 'stop'
    if ($server -and !$server.HasExited -and !(Test-Path -LiteralPath $stop)) {
        New-Item -ItemType File -Path $stop | Out-Null
        [void]$server.WaitForExit(8000)
    }
    foreach ($process in $client,$server) {
        if ($process -and !$process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) }
    }
    try {
        if ($account) {
            try {
                Get-CimInstance -ClassName Win32_UserProfile -Filter ("SID='" + $account.SID.Value + "'") | Remove-CimInstance
            } finally { Remove-LocalUser -SID $account.SID }
        }
    } finally {
        $resolved = (Resolve-Path -LiteralPath $runtime).Path
        if ((Split-Path $resolved -Parent) -ne $base -or (Get-Item -LiteralPath $resolved).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Unexpected disposable runtime path.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
$result['temporaryAccountRemoved'] = $true
$result | ConvertTo-Json -Compress
