$ErrorActionPreference = 'Stop'
$serial = New-Object System.IO.Ports.SerialPort COM1,115200,None,8,one
$serial.Open()

function Send-Result([string]$Marker, $Record) {
    $json = $Record | ConvertTo-Json -Depth 12 -Compress
    $serial.WriteLine($Marker + ' ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json)))
}

$result = [ordered]@{ok=$false; phase='language-preflight'}
$root = 'C:\xdnote-acceptance'
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $hostInfo = [ordered]@{
        caption=$os.Caption; version=$os.Version; build=$os.BuildNumber
        productType=$os.ProductType; architecture=$cpu.Architecture
        session=(Get-Process -Id $PID).SessionId
        secureBoot=(Confirm-SecureBootUEFI); tpmPresent=(Get-Tpm).TpmPresent
    }
    if ($os.ProductType -ne 1 -or $os.BuildNumber -ne '26100' -or
        $cpu.Architecture -ne 9 -or !$hostInfo.session -or
        !$hostInfo.secureBoot -or !$hostInfo.tpmPresent) {
        throw 'Expected an interactive Windows 11 x64 client with Secure Boot and TPM.'
    }
    Send-Result 'XDNOTE_VM_READY' $hostInfo
    $result.host = $hostInfo
    $languages = New-WinUserLanguageList -Language 'en-US'
    $languages.Add('zh-CN')
    Set-WinUserLanguageList -LanguageList $languages -Force
    $result.languages = @(foreach ($language in (Get-WinUserLanguageList)) {
        [ordered]@{ tag=$language.LanguageTag; inputMethodTips=@($language.InputMethodTips) }
    })
    $result.capabilities = @(Get-WindowsCapability -Online |
        Where-Object { $_.Name -like 'Language.Basic~~~zh-CN~*' } |
        ForEach-Object { [ordered]@{name=$_.Name; state=$_.State.ToString()} })
    $tips = @($result.languages | Where-Object { $_.tag -match '^zh-(Hans-)?CN$' } |
        ForEach-Object { $_.inputMethodTips })
    $result.microsoftPinyinRegistered = @($tips | Where-Object {
        $_ -match '(?i)81d4e9c9-1d3b-41bc-9e6c-4b40bf79e35e'
    }).Count -gt 0
    if (!$result.microsoftPinyinRegistered) {
        throw 'Microsoft Pinyin is not registered in the isolated Windows language profile.'
    }
    $serial.WriteLine('XDNOTE_VM_STAGE waiting-for-product')
    $deadline = [DateTime]::UtcNow.AddMinutes(30)
    do {
        $volume = Get-CimInstance Win32_LogicalDisk | Where-Object VolumeName -eq 'XDNOTE_TESTS' | Select-Object -First 1
        if ($volume) { break }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$volume) { throw 'The read-only product transfer did not arrive.' }
    $media = $volume.DeviceID + '\'
    New-Item -ItemType Directory -Path $root, "$root\scripts", "$root\build" | Out-Null
    Copy-Item -LiteralPath (Join-Path $media 'product.zip') -Destination "$root\product.zip"
    Copy-Item -LiteralPath (Join-Path $media 'ime-windows.ps1'),(Join-Path $media 'capture-windows.ps1') -Destination "$root\scripts"
    Expand-Archive -LiteralPath "$root\product.zip" -DestinationPath "$root\payload"
    $audit = Get-Content -LiteralPath (Join-Path $media 'payload-inventory.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $source = (Get-Content -LiteralPath (Join-Path $media 'source.txt') -Raw).Trim()
    if (!$audit.ok -or $audit.buildIdentity.buildVersion.revision -ne $source -or $audit.buildIdentity.buildVersion.dirty) {
        throw 'The transferred payload does not match the clean product source.'
    }
    foreach ($record in $audit.files) {
        $path = [IO.Path]::GetFullPath((Join-Path "$root\payload" $record.path))
        if (!$path.StartsWith("$root\payload\", [StringComparison]::OrdinalIgnoreCase) -or
            !(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Length -ne $record.bytes -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $record.sha256) {
            throw "The transferred product file does not match its sealed inventory: $($record.path)"
        }
    }
    if (@(Get-ChildItem -LiteralPath "$root\payload" -File -Recurse).Count -ne @($audit.files).Count) {
        throw 'The transferred product contains unexpected files.'
    }
    $result.product = [ordered]@{
        source=$source; archiveSha256=(Get-FileHash -LiteralPath "$root\product.zip" -Algorithm SHA256).Hash.ToLowerInvariant()
        files=@($audit.files).Count
    }
    $result.phase = 'real-ime'
    $serial.WriteLine('XDNOTE_VM_STAGE starting-ime')
    $env:XDNOTE_DISPOSABLE_WINDOWS_GUEST = '1'
    & "$root\scripts\ime-windows.ps1" -Payload "$root\payload" -Evidence "$root\build\evidence" `
        -FocusSignal { param([string]$request) $serial.WriteLine("XDNOTE_VM_INPUT $request") }
    $result.ok = $true
} catch {
    $result.error = $_.Exception.Message
} finally {
    $evidence = "$root\build\evidence\results.json"
    if (Test-Path -LiteralPath $evidence) {
        $result.acceptance = Get-Content -LiteralPath $evidence -Raw -Encoding UTF8 | ConvertFrom-Json
    }
    Send-Result 'XDNOTE_VM_RESULT' $result
    $serial.Close()
}
