$ErrorActionPreference = 'Stop'
$serial = New-Object System.IO.Ports.SerialPort COM1,115200,None,8,one
$serial.Open()

function Send-Result([string]$Marker, $Record) {
    $json = $Record | ConvertTo-Json -Depth 16 -Compress
    $serial.WriteLine($Marker + ' ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json)))
}

$root = 'C:\xdnote-acceptance'
$result = [ordered]@{ok=$false}
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $result.host = [ordered]@{
        caption=$os.Caption; version=$os.Version; build=$os.BuildNumber
        productType=$os.ProductType; architecture=$cpu.Architecture
        session=(Get-Process -Id $PID).SessionId
    }
    if ($os.ProductType -ne 1 -or $os.BuildNumber -ne '17763' -or $cpu.Architecture -ne 9 -or !$result.host.session) {
        throw 'Expected an interactive x64 Windows 10 build 17763 client'
    }
    Send-Result 'XDNOTE_VM_READY' $result.host
    $deadline = [DateTime]::UtcNow.AddMinutes(25)
    do {
        $volume = Get-CimInstance Win32_LogicalDisk | Where-Object VolumeName -eq 'XDNOTE_TESTS' | Select-Object -First 1
        if ($volume) { break }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$volume) { throw 'The read-only acceptance disc did not arrive' }
    $media = $volume.DeviceID + '\'
    New-Item -ItemType Directory -Path "$root\scripts","$root\build","$root\temp" | Out-Null
    Copy-Item -LiteralPath (Join-Path $media 'check_windows_file_manager.ps1') -Destination "$root\scripts"
    $env:GITHUB_ACTIONS = 'true'
    $env:RUNNER_ENVIRONMENT = 'github-hosted'
    $env:GITHUB_RUN_ID = (Get-Content -LiteralPath (Join-Path $media 'run-id.txt') -Raw).Trim()
    $env:GITHUB_SHA = (Get-Content -LiteralPath (Join-Path $media 'source.txt') -Raw).Trim()
    $env:RUNNER_TEMP = "$root\temp"
    & "$root\scripts\check_windows_file_manager.ps1" -Package (Join-Path $media 'xdnote-dev-setup.exe') `
        -Audit (Join-Path $media 'payload-inventory.json') -PackageReport (Join-Path $media 'installer-report.json') `
        -EvidenceDirectory "$root\build\evidence"
    $result.ok = $true
} catch {
    $result.error = $_.Exception.Message
} finally {
    $report = "$root\build\evidence\report.json"
    if (Test-Path -LiteralPath $report) { $result.installer = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json }
    Send-Result 'XDNOTE_VM_RESULT' $result
    $serial.Close()
}
