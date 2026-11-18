$ErrorActionPreference = 'Stop'
$repo = (Get-Location).Path
$out = Join-Path $repo 'build/evidence/reader-nvda'
$tools = Join-Path $repo 'build/reader-nvda-tools'
$config = Join-Path $tools 'config'
$portable = Join-Path $tools 'portable'
New-Item -ItemType Directory -Force -Path $out, $tools, $config | Out-Null
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

function Send-ReaderKeys([IntPtr]$window, [byte[]]$keys) {
    if ([ReaderInput]::GetForegroundWindow() -ne $window) { throw 'Fixture window lost foreground' }
    foreach ($key in $keys) {
        $extended = [uint32]($key -in @(0x24, 0x26, 0x28, 0x2D))
        [ReaderInput]::keybd_event($key, 0, $extended, [UIntPtr]::Zero)
    }
    for ($index = $keys.Count - 1; $index -ge 0; --$index) {
        $key = $keys[$index]
        $extended = [uint32]($key -in @(0x24, 0x26, 0x28, 0x2D))
        [ReaderInput]::keybd_event($key, 0, ($extended -bor 2), [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 500
}

try {
    Start-Process -FilePath $nvda -ArgumentList @('--minimal', '--disable-addons', '--no-sr-flag', '--debug-logging', '-f', ('"{0}"' -f $log), '-c', ('"{0}"' -f $config))
    Start-Sleep -Seconds 3
    $app = Start-Process -FilePath 'python' -ArgumentList @('scripts/desktop.py', 'run', '--profile', 'default', '--qt-prefix', 'build/Qt/6.8.2/msvc2022_64', '--build-dir', 'build/desktop-admission', '--file', ('"{0}"' -f $fixture)) -PassThru -RedirectStandardOutput (Join-Path $out 'app.log') -RedirectStandardError (Join-Path $out 'app.stderr.log')
    $title = "Reader-Check.md $([char]0x2014) xdnote"
    $window = [IntPtr]::Zero
    for ($attempt=0; $attempt -lt 40 -and $window -eq [IntPtr]::Zero; $attempt++) {
        $window = [ReaderInput]::FindWindow($null, $title)
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
    Start-Sleep -Seconds 6
    Send-ReaderKeys $window @(0x11, 0x41)
    Send-ReaderKeys $window @(0x2D, 0x10, 0x26)
    Send-ReaderKeys $window @(0x11, 0x24)
    Send-ReaderKeys $window @(0x09)
    Send-ReaderKeys $window @(0x2D, 0x6C)
    Send-ReaderKeys $window @(0x0D)
    Send-ReaderKeys $window @(0x2D, 0x28)
    Start-Sleep -Seconds 5
    Send-ReaderKeys $window @(0x11, 0x57)
    if ([ReaderInput]::FindWindow($null, $title) -ne [IntPtr]::Zero) { throw 'Reader window remains after close' }
    if ((Get-FileHash -LiteralPath $fixture).Hash -ne $beforeHash) { throw 'Read-only fixture changed' }
} finally {
    Start-Process -FilePath $nvda -ArgumentList @('--quit') -Wait
    if ($null -ne $app -and !$app.HasExited) { Stop-Process -Id $app.Id -ErrorAction SilentlyContinue }
}
$spoken = Get-Content -LiteralPath $log | Where-Object { $_ -match '\bSpeaking\b' }
$missing = @()
foreach ($marker in @('Blue orchard reading sample','Follow the cedar heading','Amber target reading sample')) {
    $evidence = $spoken | Where-Object { $_.Contains($marker) } | Select-Object -First 3
    if (!$evidence) { $missing += $marker }
    else { $evidence | ForEach-Object { $_.Substring(0, [Math]::Min($_.Length, 560)) } }
}
if ($missing) { throw "No genuine NVDA speech evidence for $($missing -join ', ')" }
