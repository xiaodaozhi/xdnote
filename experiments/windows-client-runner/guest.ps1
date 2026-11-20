$ErrorActionPreference = 'Stop'
$serial = New-Object System.IO.Ports.SerialPort COM1,115200,None,8,one
$serial.Open()

function Send-Result([string]$Marker, $Record) {
    $json = $Record | ConvertTo-Json -Depth 16 -Compress
    $serial.WriteLine($Marker + ' ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json)))
}

function Send-GuestStage([string]$Stage) {
    $serial.WriteLine('XDNOTE_VM_STAGE ' + $Stage)
}

$root = 'C:\xdnote-acceptance'
$result = [ordered]@{ok=$false}
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $tpm = Get-Tpm
    $result.host = [ordered]@{
        caption=$os.Caption; version=$os.Version; build=$os.BuildNumber
        productType=$os.ProductType; architecture=$cpu.Architecture
        session=(Get-Process -Id $PID).SessionId; secureBoot=(Confirm-SecureBootUEFI)
        tpmPresent=$tpm.TpmPresent
    }
    if ($os.ProductType -ne 1 -or $os.BuildNumber -ne '26100' -or $cpu.Architecture -ne 9 -or !$result.host.session -or !$result.host.secureBoot -or !$result.host.tpmPresent) {
        throw 'Expected an interactive x64 Windows 11 build 26100 client with UEFI Secure Boot and TPM'
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
    $installer = Join-Path "$root\temp" 'xdnote-dev-setup.exe'
    Copy-Item -LiteralPath (Join-Path $media 'xdnote-dev-setup.exe') -Destination $installer
    Send-GuestStage 'installer-copied'
    & "$root\scripts\check_windows_file_manager.ps1" -Package $installer `
        -Audit (Join-Path $media 'payload-inventory.json') -PackageReport (Join-Path $media 'installer-report.json') `
        -EvidenceDirectory "$root\build\evidence" -PickerOnly
    $result.ok = $true
} catch {
    $result.error = $_.Exception.Message
} finally {
    $report = "$root\build\evidence\report.json"
    if (Test-Path -LiteralPath $report) { $result.installer = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json }
    Send-Result 'XDNOTE_VM_RESULT' $result
    $serial.Close()
}
