#Requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$PayloadDir,
    [Parameter(Mandatory)]
    [string]$NvdaExe,
    [ValidateRange(10, 180)]
    [int]$ForegroundTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
$platform = [Environment]::OSVersion.Version
if (!$IsWindows -or $platform.Build -lt 22000 -or
    [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64' -or
    (Get-ItemPropertyValue 'HKLM:\SYSTEM\CurrentControlSet\Control\ProductOptions' -Name ProductType) -ne 'WinNT') {
    throw 'This check requires an interactive Windows 11 x64 client.'
}
if (Get-Process xdnote,nvda -ErrorAction SilentlyContinue) {
    throw 'Close existing xdnote and NVDA instances before running this check.'
}

$payload = (Resolve-Path -LiteralPath $PayloadDir).ProviderPath
$readerExe = (Resolve-Path -LiteralPath $NvdaExe).ProviderPath
$productExe = Join-Path $payload 'xdnote.exe'
$qtCore = Join-Path $payload 'Qt6Core.dll'
foreach ($requiredFile in @($productExe, $qtCore, (Join-Path $payload 'qt.conf'), (Join-Path $payload 'platforms/qwindows.dll'))) {
    if (!(Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Missing file in the deployed xdnote payload: $requiredFile"
    }
}
if ((Split-Path $readerExe -Leaf) -ne 'nvda.exe' -or !(Test-Path -LiteralPath $readerExe -PathType Leaf)) {
    throw 'Supply nvda.exe from a trusted portable NVDA copy.'
}

Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,WindowsBase
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class XdnoteReaderInput {
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool IsWow64Process2(IntPtr process, out ushort machine, out ushort host);

    [StructLayout(LayoutKind.Sequential)]
    public struct Point {
        public int X;
        public int Y;
    }
}
'@

$runId = '{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'), [Guid]::NewGuid().ToString('N').Substring(0, 8)
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output = Join-Path $repo "build/reader-acceptance/windows-nvda/$runId"
$config = Join-Path $output 'nvda-config'
$pluginDir = Join-Path $config 'scratchpad/globalPlugins'
$fixture = Join-Path $output "NVDA reader check $runId.md"
$proof = Join-Path $output 'speech-markers.txt'
$phaseFile = Join-Path $config 'current-phase.txt'
$ready = Join-Path $config 'ready'
New-Item -ItemType Directory -Path $pluginDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'nvda_fixture_speech.py') -Destination $pluginDir

@(
    '# Opening Heading', '', 'Opening reading marker. Violet river selection.', '',
    '[Jump to the distant target](#distant-target-heading)', '',
    (1..85 | ForEach-Object { "Intervening filler paragraph $_. This paragraph keeps the destination below the initial viewport."; '' }),
    '## Distant Target Heading', '', 'The destination paragraph is now being read. Blue harbor continuation.', '',
    '[Return to the opening heading](#opening-heading)'
) | Set-Content -LiteralPath $fixture -Encoding utf8NoBOM
$beforeHash = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash.ToLowerInvariant()
'' | Set-Content -LiteralPath $proof -Encoding utf8NoBOM
@'
[general]
    saveConfigurationOnExit = False
[mouse]
    enableMouseTracking = False
[update]
    autoCheck = False
    startupNotification = False
[addonStore]
    automaticUpdates = disabled
[development]
    enableScratchpadDir = True
'@ | Set-Content -LiteralPath (Join-Path $config 'nvda.ini') -Encoding utf8NoBOM

$keyCodes = @{
    Control = 0x11; Shift = 0x10; Alt = 0x12; Tab = 0x09; Home = 0x24; End = 0x23
    Up = 0x26; Down = 0x28; Insert = 0x2D; KeypadEnter = 0x0D; F4 = 0x73
}
$extendedKeys = @('Home', 'End', 'Up', 'Down', 'Insert', 'KeypadEnter')
$windows = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -Name DisplayVersion,UBR
$environment = [ordered]@{
    windows = $windows.DisplayVersion
    windowsVersion = "10.0.$($platform.Build).$($windows.UBR)"
    architecture = 'x64'
    application = $null
    applicationSha256 = (Get-FileHash -LiteralPath $productExe -Algorithm SHA256).Hash.ToLowerInvariant()
    qt = (Get-Item -LiteralPath $qtCore).VersionInfo.FileVersion
    nvda = (Get-Item -LiteralPath $readerExe).VersionInfo.ProductVersion
}
$checks = [ordered]@{
    body = $false; selection = $false; linkFocus = $false; target = $false
    edit = $false; readAgain = $false; closed = $false; hashUnchanged = $false; nvdaStopped = $false
}
$report = [ordered]@{
    passed = $false; environment = $environment; checks = $checks; error = $null
    fixtureSha256Before = $beforeHash; fixtureSha256After = $null
}

function Set-Phase([string]$phase) {
    Set-Content -LiteralPath $phaseFile -Value $phase -Encoding ascii
}

function Assert-FixtureForeground {
    if ([XdnoteReaderInput]::GetForegroundWindow() -ne $window) {
        throw 'The xdnote fixture lost foreground. Stopped sending input.'
    }
}

function Send-ReaderKeys([string[]]$keys) {
    Assert-FixtureForeground
    foreach ($key in $keys) {
        [XdnoteReaderInput]::keybd_event($keyCodes[$key], 0, [uint32]($key -in $extendedKeys), [UIntPtr]::Zero)
    }
    for ($index = $keys.Count - 1; $index -ge 0; --$index) {
        $key = $keys[$index]
        $flags = [uint32]($key -in $extendedKeys) -bor 2
        [XdnoteReaderInput]::keybd_event($keyCodes[$key], 0, $flags, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 350
}

function Click-FixtureControl([string]$name, [switch]$Viewport) {
    $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $name)
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Assert-FixtureForeground
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($window)
        $visible = @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition) | Where-Object { !$_.Current.IsOffscreen })
        if ($visible.Count -gt 0) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($visible.Count -ne 1) { throw "Expected one visible fixture control named '$name'; found $($visible.Count)." }

    $bounds = $visible[0].Current.BoundingRectangle
    $point = [XdnoteReaderInput+Point]::new()
    if ($Viewport) {
        # Give the text viewport focus without hitting the inline link or scrollbars.
        $point.X = [int]($bounds.Left + [Math]::Min(26, $bounds.Width / 4))
        $point.Y = [int]($bounds.Top + [Math]::Min(35, $bounds.Height / 4))
    } else {
        $point.X = [int]($bounds.Left + $bounds.Width / 2)
        $point.Y = [int]($bounds.Top + $bounds.Height / 2)
    }
    Assert-FixtureForeground
    if ([XdnoteReaderInput]::GetAncestor([XdnoteReaderInput]::WindowFromPoint($point), 2) -ne $window) {
        throw "The fixture control is obscured: $name"
    }
    $cursor = [XdnoteReaderInput+Point]::new()
    if (![XdnoteReaderInput]::GetCursorPos([ref]$cursor) -or ![XdnoteReaderInput]::SetCursorPos($point.X, $point.Y)) {
        throw 'Could not move the pointer to the fixture.'
    }
    try {
        Assert-FixtureForeground
        [XdnoteReaderInput]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
        [XdnoteReaderInput]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    } finally {
        [void][XdnoteReaderInput]::SetCursorPos($cursor.X, $cursor.Y)
    }
    Start-Sleep -Milliseconds 450
}

function Wait-SpeechMarker([string]$pattern, [int]$seconds = 4) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    do {
        Assert-FixtureForeground
        if (Select-String -LiteralPath $proof -Pattern $pattern -Quiet) { return $true }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

$app = $null
$reader = $null
$startLog = Join-Path $output 'nvda-startup.log'
$window = [IntPtr]::Zero
Write-Host "Evidence directory: $output"
try {
    $identityFile = Join-Path $output 'xdnote-version.json'
    $identityProcess = Start-Process -FilePath $productExe -ArgumentList '--version-json' -WorkingDirectory $payload -WindowStyle Hidden -PassThru -RedirectStandardOutput $identityFile
    if (!$identityProcess.WaitForExit(8000)) {
        $identityProcess.Kill()
        throw 'xdnote did not finish reporting its build identity.'
    }
    if ($identityProcess.ExitCode -ne 0) { throw 'xdnote could not report its build identity.' }
    $environment.application = Get-Content -Raw -LiteralPath $identityFile | ConvertFrom-Json
    if ($environment.application.product -ne 'xdnote' -or !$environment.application.version) {
        throw 'The payload did not report an xdnote build identity.'
    }

    $app = Start-Process -FilePath $productExe -ArgumentList @('--', ('"' + $fixture + '"')) -WorkingDirectory $payload -PassThru
    [uint16]$machine = 0
    [uint16]$hostMachine = 0
    if (![XdnoteReaderInput]::IsWow64Process2($app.Handle, [ref]$machine, [ref]$hostMachine) -or $machine -ne 0 -or $hostMachine -ne 0x8664) {
        throw 'Expected an AMD64 xdnote process running natively.'
    }
    $title = Split-Path $fixture -Leaf
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ($app.HasExited) { throw "xdnote exited before its fixture window opened ($($app.ExitCode))." }
        $app.Refresh()
        if ($app.MainWindowTitle.StartsWith($title, [StringComparison]::Ordinal)) {
            $window = $app.MainWindowHandle
            break
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero) { throw 'The standalone xdnote fixture window did not open.' }
    $app.Id | Set-Content -LiteralPath (Join-Path $config 'fixture-pid.txt') -Encoding ascii

    [void][XdnoteReaderInput]::SetForegroundWindow($window)
    if ([XdnoteReaderInput]::GetForegroundWindow() -ne $window) {
        Write-Host "Bring '$title' to the foreground within $ForegroundTimeoutSeconds seconds."
        $deadline = [DateTime]::UtcNow.AddSeconds($ForegroundTimeoutSeconds)
        while ([XdnoteReaderInput]::GetForegroundWindow() -ne $window -and [DateTime]::UtcNow -lt $deadline -and !$app.HasExited) {
            Start-Sleep -Milliseconds 100
        }
    }
    Assert-FixtureForeground
    if (Get-Process nvda -ErrorAction SilentlyContinue) { throw 'Another NVDA instance started before the test.' }

    Set-Phase setup
    $reader = Start-Process -FilePath $readerExe -WorkingDirectory (Split-Path $readerExe) -WindowStyle Hidden -PassThru -ArgumentList @(
        '--minimal', '--lang=en', '--no-sr-flag', '--log-level=20', ('--config-path="' + $config + '"'), ('--log-file="' + $startLog + '"')
    )
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (!(Test-Path -LiteralPath $ready) -and !$reader.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Assert-FixtureForeground
        Start-Sleep -Milliseconds 100
    }
    if (!(Test-Path -LiteralPath $ready) -or $reader.HasExited) { throw 'NVDA did not load the fixture speech observer.' }

    Set-Phase body
    Click-FixtureControl '阅读'
    Click-FixtureControl 'Markdown 阅读内容' -Viewport
    Send-ReaderKeys Control,Home
    Send-ReaderKeys Insert,Down
    $checks.body = Wait-SpeechMarker '^body: .*body'
    Send-ReaderKeys Control

    Set-Phase select-heading
    Send-ReaderKeys Control,Home
    Send-ReaderKeys Shift,End
    Set-Phase report-selection
    Send-ReaderKeys Insert,Shift,Up
    $checks.selection = Wait-SpeechMarker '^report-selection: .*opening.*selected'

    Send-ReaderKeys Control,Home
    Set-Phase link-focus
    Send-ReaderKeys Tab
    $checks.linkFocus = Wait-SpeechMarker '^link-focus: .*link'

    if ($checks.linkFocus) {
        Set-Phase activation
        Send-ReaderKeys Insert,KeypadEnter
        Set-Phase say-all-target
        Send-ReaderKeys Insert,Down
        $gotContinuation = Wait-SpeechMarker '^say-all-target: .*continuation' 8
        $targetSpeech = @(Get-Content -LiteralPath $proof | Where-Object { $_ -like 'say-all-target:*' })
        $checks.target = $gotContinuation -and $targetSpeech[0] -match '^say-all-target: target(,|$)' -and
            !($targetSpeech -match 'body|opening|filler')
        Send-ReaderKeys Control
    }

    Set-Phase edit
    Click-FixtureControl '编辑'
    Click-FixtureControl 'Markdown 源文' -Viewport
    Send-ReaderKeys Insert,Tab
    $checks.edit = Wait-SpeechMarker '^edit: .*source'

    Set-Phase read-again
    Click-FixtureControl '阅读'
    Click-FixtureControl 'Markdown 阅读内容' -Viewport
    Send-ReaderKeys Control,Home
    Send-ReaderKeys Insert,Down
    $checks.readAgain = Wait-SpeechMarker '^read-again: .*body'
    Send-ReaderKeys Control

    Set-Phase close
    Send-ReaderKeys Alt,F4
    $checks.closed = $app.WaitForExit(5000) -and $app.ExitCode -eq 0
} catch {
    $report.error = $_.Exception.Message
} finally {
    if ($reader -and !$reader.HasExited) {
        try {
            $quit = Start-Process -FilePath $readerExe -WindowStyle Hidden -ArgumentList '--quit' -PassThru
            $checks.nvdaStopped = $reader.WaitForExit(5000) -and $reader.ExitCode -eq 0
            [void]$quit.WaitForExit(3000)
        } catch {
            $report.error ??= $_.Exception.Message
        } finally {
            if (!$reader.HasExited) { $reader.Kill() }
        }
    }
    if ($app -and !$app.HasExited -and $window -ne [IntPtr]::Zero) {
        $app.Refresh()
        # Single-instance routing can receive another document during the run.
        # Close only the fixture through its normal window-close path.
        if ($app.MainWindowHandle -eq $window) {
            [void]$app.CloseMainWindow()
        }
    }
    if (Test-Path -LiteralPath $fixture -PathType Leaf) {
        $report.fixtureSha256After = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash.ToLowerInvariant()
        $checks.hashUnchanged = $report.fixtureSha256After -eq $beforeHash
    }
    $report.passed = $null -eq $report.error -and !($checks.Values -contains $false)
    $report | ConvertTo-Json -Depth 5 | Tee-Object -FilePath (Join-Path $output 'results.json')
}
if (!$report.passed) { exit 1 }
