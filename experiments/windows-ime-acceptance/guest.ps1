$ErrorActionPreference = 'Stop'
$serial = New-Object System.IO.Ports.SerialPort COM1,115200,None,8,one
$serial.Open()

function Send-Result([string]$Marker, $Record) {
    $json = $Record | ConvertTo-Json -Depth 12 -Compress
    $serial.WriteLine($Marker + ' ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json)))
}

$result = [ordered]@{ok=$false; phase='language-preflight'}
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
    $result.languages = @(Get-WinUserLanguageList | ForEach-Object {
        [ordered]@{ tag=$_.LanguageTag; inputMethodTips=@($_.InputMethodTips) }
    })
    $result.capabilities = @(Get-WindowsCapability -Online |
        Where-Object { $_.Name -like 'Language.Basic~~~zh-CN~*' } |
        ForEach-Object { [ordered]@{name=$_.Name; state=$_.State.ToString()} })
    $tips = @($result.languages | Where-Object { $_.tag -eq 'zh-CN' } |
        ForEach-Object { $_.inputMethodTips })
    $result.microsoftPinyinRegistered = @($tips | Where-Object {
        $_ -match '(?i)81d4e9c9-1d3b-41bc-9e6c-4b40bf79e35e'
    }).Count -gt 0
    if (!$result.microsoftPinyinRegistered) {
        throw 'Microsoft Pinyin is not registered in the isolated Windows language profile.'
    }
    $result.ok = $true
} catch {
    $result.error = $_.Exception.Message
} finally {
    Send-Result 'XDNOTE_VM_RESULT' $result
    $serial.Close()
}
