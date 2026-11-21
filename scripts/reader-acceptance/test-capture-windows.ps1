$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/capture-windows.ps1"
$root = Join-Path ([IO.Path]::GetTempPath()) ("xdnote-stream-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$capture = $null
try {
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $args = '-NoProfile -NonInteractive -Command "[Console]::Error.WriteLine(''xdnote-stream-probe''); Start-Sleep -Seconds 30"'
    $capture = [XdnoteCapturedProcess]::Start($powershell, $args, $root, "$root\stderr.log", "$root\stdout.log")
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ($capture.InputLog() -notmatch 'xdnote-stream-probe' -and [DateTime]::UtcNow -lt $deadline) {
        if ($capture.Process.HasExited) { throw 'The stream probe exited before its live log was read.' }
        Start-Sleep -Milliseconds 100
    }
    if ($capture.InputLog() -notmatch 'xdnote-stream-probe' -or $capture.Process.HasExited) {
        throw 'Could not read the stream probe while its process was running.'
    }
    $capture.Process.Kill()
    $capture.Process.WaitForExit()
    $capture.Dispose()
    $capture = $null
    if ([IO.File]::ReadAllText("$root\stderr.log") -notmatch 'xdnote-stream-probe') {
        throw 'The persisted stream probe is missing.'
    }
    Write-Host 'Live and persisted process stream capture passed.'
} finally {
    if ($capture) {
        if (!$capture.Process.HasExited) { $capture.Process.Kill(); $capture.Process.WaitForExit() }
        $capture.Dispose()
    }
    Remove-Item -LiteralPath $root -Recurse -Force
}
