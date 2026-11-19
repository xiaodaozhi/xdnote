$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'This script expects an isolated GitHub-hosted desktop'
}
$repo = (Get-Location).Path
$out = Join-Path $repo 'build/evidence/reader-nvda'
$tools = Join-Path $repo 'build/reader-nvda-tools'
$config = Join-Path $tools 'config'
$portable = Join-Path $tools 'portable'
New-Item -ItemType Directory -Force -Path $out, $tools, $config | Out-Null
$system = Get-CimInstance Win32_OperatingSystem
if ($system.ProductType -ne 1 -or [int]$system.BuildNumber -lt 22000) {
    throw 'Windows 11 client is required'
}
$installed = Join-Path $tools 'installed-xdnote'
$setup = Join-Path $repo 'build/reader-win11-package/xdnote-dev-setup.exe'
$receipt = Get-Content -Raw -LiteralPath (Join-Path $repo 'build/reader-win11-package/installer-report.json') | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $setup).Hash -ne $receipt.sha256) {
    throw 'xdnote package differs from the same-run installer receipt'
}
$env:QT_PLUGIN_PATH = $null
$env:QML_IMPORT_PATH = $null
$env:QML2_IMPORT_PATH = $null
$fixture = Join-Path $out 'Reader-Check.md'
@(
    '# Native reading acceptance', '', 'Blue orchard reading sample.', '',
    '[Follow the cedar heading](#cedar-anchor)', '',
    (1..40 | ForEach-Object { "Filler paragraph $_."; '' }),
    '## Cedar anchor', '', 'Amber target reading sample.'
) | Set-Content -LiteralPath $fixture -Encoding utf8
$beforeHash = (Get-FileHash -LiteralPath $fixture).Hash

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class ReaderInput {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")]
    public static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")]
    public static extern bool CloseDesktop(IntPtr handle);
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);
    [StructLayout(LayoutKind.Sequential)]
    public struct Point { public int X; public int Y; }
}
"@
$desktop = [ReaderInput]::OpenInputDesktop(0, $false, 1)
if ($desktop -eq [IntPtr]::Zero) { throw 'Hosted runner has no accessible input desktop' }
[void][ReaderInput]::CloseDesktop($desktop)

$installer = Join-Path $tools 'nvda_2024.4.2.exe'
Invoke-WebRequest -Uri 'https://download.nvaccess.org/releases/2024.4.2/nvda_2024.4.2.exe' -OutFile $installer
if ((Get-FileHash -LiteralPath $installer).Hash -ne 'b66bbc3d87f11140fe9493ca22955c401c0b09abd0bf3c9d49260807986d4f05') {
    throw 'NVDA installer hash differs from the publisher record'
}
$install = Start-Process -FilePath $installer -ArgumentList @('--create-portable-silent', '--portable-path', ('"{0}"' -f $portable)) -PassThru -Wait
if ($install.ExitCode -ne 0) { throw "NVDA portable creation failed ($($install.ExitCode))" }
$nvda = Join-Path $portable 'nvda.exe'
if (!(Test-Path -LiteralPath $nvda)) { throw 'NVDA portable executable missing' }
$log = Join-Path $out 'nvda.log'
@'
[update]
    autoCheck = False
'@ | Set-Content -LiteralPath (Join-Path $config 'nvda.ini') -Encoding ascii

function Send-ReaderKeys([IntPtr]$window, [byte[]]$keys, [bool]$keypadEnter = $false) {
    if ([ReaderInput]::GetForegroundWindow() -ne $window) { throw 'Fixture window lost foreground' }
    foreach ($key in $keys) {
        $extended = [uint32](($key -in @(0x24, 0x26, 0x28, 0x2D)) -or ($keypadEnter -and $key -eq 0x0D))
        [ReaderInput]::keybd_event($key, 0, $extended, [UIntPtr]::Zero)
    }
    for ($index = $keys.Count - 1; $index -ge 0; --$index) {
        $key = $keys[$index]
        $extended = [uint32](($key -in @(0x24, 0x26, 0x28, 0x2D)) -or ($keypadEnter -and $key -eq 0x0D))
        [ReaderInput]::keybd_event($key, 0, ($extended -bor 2), [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 500
}

$app = $null
$xdnoteInstalled = $false
try {
    $setupArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LANG=english', '/TASKS=', ('/DIR="{0}"' -f $installed), ('/LOG="{0}"' -f (Join-Path $out 'setup.log')))
    $setupProcess = Start-Process -FilePath $setup -ArgumentList $setupArgs -Wait -PassThru
    if ($setupProcess.ExitCode -ne 0) { throw "xdnote installation failed ($($setupProcess.ExitCode))" }
    $xdnoteInstalled = $true
    Start-Process -FilePath $nvda -ArgumentList @('--minimal', '--disable-addons', '--no-sr-flag', '--debug-logging', '-f', ('"{0}"' -f $log), '-c', ('"{0}"' -f $config))
    Start-Sleep -Seconds 3
    $app = Start-Process -FilePath (Join-Path $installed 'xdnote.exe') -WorkingDirectory $installed -ArgumentList @(('"{0}"' -f $fixture)) -PassThru -RedirectStandardOutput (Join-Path $out 'app.log') -RedirectStandardError (Join-Path $out 'app.stderr.log')
    [uint16]$applicationMachine = 0
    [uint16]$hostMachine = 0
    if (![ReaderInput]::IsWow64Process2($app.Handle, [ref]$applicationMachine, [ref]$hostMachine) -or $hostMachine -ne 0xaa64 -or $applicationMachine -ne 0x8664) {
        throw 'Expected an AMD64 xdnote process under Windows ARM64 emulation'
    }
    "Windows client $($system.Version), native host ARM64, xdnote process AMD64 via emulation, installer SHA256 $($receipt.sha256)" |
        Tee-Object -FilePath (Join-Path $out 'environment.log')
    $title = "Reader-Check.md $([char]0x2014) xdnote"
    $window = [IntPtr]::Zero
    for ($attempt=0; $attempt -lt 40 -and $window -eq [IntPtr]::Zero; $attempt++) {
        $window = [ReaderInput]::FindWindow([NullString]::Value, $title)
        if ($window -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
    }
    if ($window -eq [IntPtr]::Zero) { throw 'xdnote reader fixture window did not appear' }
    [void][ReaderInput]::ShowWindow($window, 9)
    [void][ReaderInput]::SetForegroundWindow($window)
    if ([ReaderInput]::GetForegroundWindow() -ne $window) { throw 'xdnote fixture is not foreground' }
    $point = New-Object ReaderInput+Point
    $point.X = 120
    $point.Y = 225
    if (![ReaderInput]::ClientToScreen($window, [ref]$point)) { throw 'Cannot locate document region' }
    [void][ReaderInput]::SetCursorPos($point.X, $point.Y)
    [ReaderInput]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [ReaderInput]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Send-ReaderKeys $window @(0x11, 0x24)
    Send-ReaderKeys $window @(0x2D, 0x28)
    Start-Sleep -Seconds 2
    Send-ReaderKeys $window @(0x28)
    Send-ReaderKeys $window @(0x2D, 0x26)
    $bodyEnd = (Get-Item -LiteralPath $log).Length
    Send-ReaderKeys $window @(0x11, 0x41)
    Send-ReaderKeys $window @(0x2D, 0x10, 0x26)
    $selectionEnd = (Get-Item -LiteralPath $log).Length
    Send-ReaderKeys $window @(0x11, 0x24)
    $focusStart = (Get-Item -LiteralPath $log).Length
    Send-ReaderKeys $window @(0x09)
    Start-Sleep -Milliseconds 400
    $focusEnd = (Get-Item -LiteralPath $log).Length
    Send-ReaderKeys $window @(0x2D, 0x0D) $true
    Send-ReaderKeys $window @(0x2D, 0x28)
    Start-Sleep -Seconds 5
    $targetEnd = (Get-Item -LiteralPath $log).Length
    Send-ReaderKeys $window @(0x12, 0x73)
    for ($attempt=0; $attempt -lt 30 -and [ReaderInput]::FindWindow([NullString]::Value, $title) -ne [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 100
    }
    if ([ReaderInput]::FindWindow([NullString]::Value, $title) -ne [IntPtr]::Zero) { throw 'Reader window remains after Alt+F4' }
    if ((Get-FileHash -LiteralPath $fixture).Hash -ne $beforeHash) { throw 'Read-only fixture changed' }
} finally {
    Start-Process -FilePath $nvda -ArgumentList @('--quit') -Wait
    if ($null -ne $app -and !$app.HasExited) {
        Stop-Process -Id $app.Id -ErrorAction SilentlyContinue
        [void]$app.WaitForExit(5000)
    }
    if ($xdnoteInstalled) {
        $uninstall = Start-Process -FilePath (Join-Path $installed 'unins000.exe') -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -Wait -PassThru
        if ($uninstall.ExitCode -ne 0) { throw "xdnote cleanup failed ($($uninstall.ExitCode))" }
    }
}
$bytes = [IO.File]::ReadAllBytes($log)
$phases = @(
    @{ Label='Body reading'; Start=0; End=$bodyEnd; Marker='Blue orchard reading sample' },
    @{ Label='Text selection'; Start=$bodyEnd; End=$selectionEnd; Marker='selected' },
    @{ Label='Link focus'; Start=$focusStart; End=$focusEnd; Marker='Follow the cedar heading' },
    @{ Label='Reading from target'; Start=$focusEnd; End=$targetEnd; Marker='Amber target reading sample' }
)
$missing = @()
foreach ($phase in $phases) {
    $data = [Text.Encoding]::UTF8.GetString($bytes, $phase.Start, ($phase.End - $phase.Start))
    $evidence = ($data -split "`n") | Where-Object { $_ -match '\bSpeaking\b' -and $_.ToLower().Contains($phase.Marker.ToLower()) } | Select-Object -First 2
    if (!$evidence) { $missing += $phase.Label }
    else { $evidence | ForEach-Object { $_.Substring(0, [Math]::Min($_.Length, 560)) } }
}
if ($missing) { throw "No genuine NVDA speech evidence for $($missing -join ', ')" }
