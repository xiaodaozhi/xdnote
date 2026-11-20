param(
    [Parameter(Mandatory = $true)][string]$Payload,
    [Parameter(Mandatory = $true)][string]$Evidence,
    [Parameter(Mandatory = $true)][scriptblock]$FocusSignal
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/capture-windows.ps1"
if ($env:XDNOTE_DISPOSABLE_WINDOWS_GUEST -ne '1' -or
    (Get-Process -Id $PID).SessionId -eq 0) {
    throw 'This check requires the isolated interactive Windows client fixture.'
}
$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
if ($os.ProductType -ne 1 -or $os.BuildNumber -lt 22000 -or $cpu.Architecture -ne 9) {
    throw 'Expected an interactive Windows 11 x64 client.'
}
if (Get-Process xdnote -ErrorAction SilentlyContinue) {
    throw 'A previous xdnote process is running in the isolated session.'
}

$payload = (Resolve-Path -LiteralPath $Payload).ProviderPath
$guestRoot = [IO.Path]::GetFullPath('C:\xdnote-acceptance\')
if (!$payload.StartsWith($guestRoot, [StringComparison]::OrdinalIgnoreCase) -or
    ![IO.Path]::GetFullPath($Evidence).StartsWith($guestRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The product and evidence must belong to the disposable guest fixture.'
}
$exe = Join-Path $payload 'xdnote.exe'
$core = Join-Path $payload 'Qt6Core.dll'
foreach ($file in @($exe, $core, (Join-Path $payload 'qt.conf'), (Join-Path $payload 'platforms\qwindows.dll'))) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing deployed product file: $file" }
}
if (Test-Path -LiteralPath $Evidence) { throw 'Choose a new evidence directory.' }
$evidence = [IO.Path]::GetFullPath($Evidence)
New-Item -ItemType Directory -Path $evidence, "$evidence\workspace", "$evidence\profile" | Out-Null
$fixture = Join-Path $evidence 'workspace\IME-Check.md'
$original = "# Input acceptance`n`nSeed: "
$chinese = [regex]::Unescape('\u4f60\u597d')
$committed = $original + $chinese
[IO.File]::WriteAllText($fixture, $original, [Text.UTF8Encoding]::new($false))
$before = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash.ToLowerInvariant()

Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class XdnoteImeInput {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint thread);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rectangle rectangle);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct Rectangle { public int Left; public int Top; public int Right; public int Bottom; }
}
'@

$report = [ordered]@{
    passed=$false; phase='setup'; environment=[ordered]@{
        windows=$os.Version; build=$os.BuildNumber; architecture='x64'
        executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        qtCoreSha256=(Get-FileHash -LiteralPath $core -Algorithm SHA256).Hash.ToLowerInvariant()
        qtVersion=(Get-Item -LiteralPath $core).VersionInfo.FileVersion
    }
    checks=[ordered]@{
        workspaceOpened=$false; focusedStandaloneEditor=$false; microsoftPinyin=$false
        nativePreedit=$false; nativeCommit=$false; workspaceTransfer=$false
        undo=$false; redo=$false; selection=$false; diskUnchanged=$false
    }
    fixtureSha256Before=$before; fixtureSha256After=$null; nativeInputEvents=@(); error=$null
}
$log = Join-Path $evidence 'qt-input.log'
$stdout = Join-Path $evidence 'xdnote-stdout.log'
$app = $null
$capture = $null

function Find-Window([string]$prefix) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $app.Id)
    $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $condition)
    foreach ($window in $windows) {
        if (!$window.Current.IsOffscreen -and $window.Current.Name.StartsWith($prefix)) { return $window }
    }
    return $null
}

function Wait-Window([string]$prefix) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if ($app.HasExited) { throw 'The owned xdnote process exited unexpectedly.' }
        $window = Find-Window $prefix
        if ($window) { return $window }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Could not find the owned window: $prefix"
}

function Find-Control($window, [string]$name) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::NameProperty, $name)
    $items = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
    $visible = @($items | Where-Object { !$_.Current.IsOffscreen })
    if ($visible.Count -ne 1) { throw "Expected one visible owned control named $name; got $($visible.Count)." }
    return $visible[0]
}

function Activate-Window($window) {
    $handle = [IntPtr]$window.Current.NativeWindowHandle
    if ($handle -eq [IntPtr]::Zero) { throw 'The owned window has no native handle.' }
    [void][XdnoteImeInput]::SetForegroundWindow($handle)
    if ([XdnoteImeInput]::GetForegroundWindow() -ne $handle) {
        [uint32]$foregroundId = 0
        [void][XdnoteImeInput]::GetWindowThreadProcessId([XdnoteImeInput]::GetForegroundWindow(), [ref]$foregroundId)
        $foreground = Get-Process -Id $foregroundId -ErrorAction SilentlyContinue
        if ($foreground -and $foreground.ProcessName -in @('StartMenuExperienceHost', 'SearchHost')) {
            & $FocusSignal 'escape'
            Start-Sleep -Milliseconds 1000
        }
        [void][XdnoteImeInput]::SetForegroundWindow($handle)
    }
    if ([XdnoteImeInput]::GetForegroundWindow() -ne $handle) {
        $rect = [XdnoteImeInput+Rectangle]::new()
        if (![XdnoteImeInput]::GetWindowRect($handle, [ref]$rect)) { throw 'Could not inspect the owned window.' }
        $point = [XdnoteImeInput+Point]::new()
        $point.X = [int](($rect.Left + $rect.Right) / 2)
        $point.Y = $rect.Top + 12
        $width = [XdnoteImeInput]::GetSystemMetrics(0)
        $height = [XdnoteImeInput]::GetSystemMetrics(1)
        if ($point.X -lt 0 -or $point.X -ge $width -or $point.Y -lt 0 -or $point.Y -ge $height -or
            [XdnoteImeInput]::GetAncestor([XdnoteImeInput]::WindowFromPoint($point), 2) -ne $handle) {
            throw 'The owned window title bar is obscured or outside the guest screen.'
        }
        & $FocusSignal ("click $($point.X) $($point.Y) $width $height")
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    while ([XdnoteImeInput]::GetForegroundWindow() -ne $handle -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if ([XdnoteImeInput]::GetForegroundWindow() -ne $handle) { throw 'The owned window did not receive foreground focus.' }
    return $handle
}

function Click-Control($window, [string]$name) {
    $handle = Activate-Window $window
    $element = Find-Control $window $name
    $rect = $element.Current.BoundingRectangle
    if ($rect.IsEmpty -or $rect.Width -lt 2 -or $rect.Height -lt 2) { throw "Control has no visible bounds: $name" }
    $point = [XdnoteImeInput+Point]::new()
    $point.X = [int]($rect.Left + $rect.Width / 2)
    $point.Y = [int]($rect.Top + $rect.Height / 2)
    if ([XdnoteImeInput]::GetAncestor([XdnoteImeInput]::WindowFromPoint($point), 2) -ne $handle) {
        throw "The owned control is obscured: $name"
    }
    if (![XdnoteImeInput]::SetCursorPos($point.X, $point.Y)) { throw 'Could not position the guest pointer.' }
    [XdnoteImeInput]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [XdnoteImeInput]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
}

function Send-Keys([IntPtr]$handle, [byte[]]$keys) {
    if ([XdnoteImeInput]::GetForegroundWindow() -ne $handle) { throw 'The owned window lost foreground focus.' }
    foreach ($key in $keys) {
        $extended = [uint32]($key -in @(0x23,0x25,0x5b))
        [XdnoteImeInput]::keybd_event($key, 0, $extended, [UIntPtr]::Zero)
    }
    for ($i = $keys.Count - 1; $i -ge 0; --$i) {
        $extended = [uint32]($keys[$i] -in @(0x23,0x25,0x5b))
        [XdnoteImeInput]::keybd_event($keys[$i], 0, ($extended -bor 2), [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 180
}

function Wait-Value($editor, [string]$expected) {
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
        $value = $editor.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        if ($value.Current.Value -ceq $expected) { return }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'The owned editor did not reach the expected document value.'
}

function Read-InputLog {
    if (!$capture) { return '' }
    return $capture.InputLog()
}

function Wait-InputLog([string]$pattern, [int]$offset) {
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    do {
        $content = Read-InputLog
        if ($content.Length -ge $offset -and $content.Substring($offset) -match $pattern) { return }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "The native Qt input log did not contain $pattern."
}

try {
    $tips = @(foreach ($language in (Get-WinUserLanguageList)) {
        if ($language.LanguageTag -match '^zh-(Hans-)?CN$') { $language.InputMethodTips }
    })
    if ($tips.Count -ne 1 -or @($tips | Where-Object {
        $_ -match '(?i)81d4e9c9-1d3b-41bc-9e6c-4b40bf79e35e'
    }).Count -ne 1) {
        throw 'Microsoft Pinyin is not registered in the isolated user profile.'
    }
    $report.environment.inputMethodTips = $tips
    $env:QT_LOGGING_RULES = 'qt.qpa.input.methods.debug=true'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $env:APPDATA = "$evidence\profile\roaming"
    $env:LOCALAPPDATA = "$evidence\profile\local"
    New-Item -ItemType Directory -Path $env:APPDATA,$env:LOCALAPPDATA -Force | Out-Null
    $capture = [XdnoteCapturedProcess]::Start($exe, '', $payload, $log, $stdout)
    $app = $capture.Process
    $report.phase = 'workspace'
    $workspace = Wait-Window 'xdnote'
    Click-Control $workspace ([regex]::Unescape('\u6253\u5f00\u6587\u4ef6\u5939'))
    $report.phase = 'directory-picker'
    $dialog = Wait-Window ([regex]::Unescape('\u6253\u5f00\u5de5\u4f5c\u533a\u76ee\u5f55'))
    $dialogHandle = Activate-Window $dialog
    [System.Windows.Forms.Clipboard]::SetText((Split-Path $fixture))
    Send-Keys $dialogHandle @(0x11,0x4c)
    Send-Keys $dialogHandle @(0x11,0x56)
    Send-Keys $dialogHandle @(0x0d)
    Start-Sleep -Milliseconds 800
    $dialog = Wait-Window ([regex]::Unescape('\u6253\u5f00\u5de5\u4f5c\u533a\u76ee\u5f55'))
    $buttons = @('Select Folder', [regex]::Unescape('\u9009\u62e9\u6587\u4ef6\u5939'))
    $chosen = $false
    foreach ($button in $buttons) {
        try { Click-Control $dialog $button; $chosen = $true; break } catch { }
    }
    if (!$chosen) { throw 'The owned native directory picker has no Select Folder button.' }
    $workspace = Wait-Window 'workspace'
    $report.checks.workspaceOpened = $true
    $report.phase = 'standalone'
    $forward = Start-Process -FilePath $exe -ArgumentList @('--', ('"' + $fixture + '"')) -WorkingDirectory $payload -PassThru
    if (!$forward.WaitForExit(10000) -or $forward.ExitCode -ne 0) { throw 'The file-open request was not forwarded.' }
    $standalone = Wait-Window 'IME-Check.md'
    Click-Control $standalone ([regex]::Unescape('\u7f16\u8f91'))
    $editor = Find-Control $standalone ([regex]::Unescape('Markdown \u6e90\u6587'))
    $editor.SetFocus()
    $windowHandle = Activate-Window $standalone
    Wait-Value $editor $original
    $report.checks.focusedStandaloneEditor = $editor.Current.HasKeyboardFocus
    if (!$report.checks.focusedStandaloneEditor) { throw 'The standalone editor did not receive keyboard focus.' }

    $report.phase = 'microsoft-pinyin'
    [uint32]$processId = 0
    $thread = [XdnoteImeInput]::GetWindowThreadProcessId($windowHandle, [ref]$processId)
    for ($attempt = 0; $attempt -lt 3; ++$attempt) {
        $hkl = [XdnoteImeInput]::GetKeyboardLayout($thread).ToInt64()
        if (($hkl -band 0xffff) -eq 0x0804) { break }
        Send-Keys $windowHandle @(0x5b,0x20)
        Start-Sleep -Milliseconds 600
    }
    if (($hkl -band 0xffff) -ne 0x0804) { throw 'The owned editor did not switch to the Chinese input profile.' }
    $report.environment.keyboardLayout = ('0x{0:X}' -f $hkl)
    $report.checks.microsoftPinyin = $true
    Send-Keys $windowHandle @(0x11,0x23)
    $inputOffset = (Read-InputLog).Length
    foreach ($key in @(0x4e,0x49,0x48,0x41,0x4f)) { Send-Keys $windowHandle @($key) }
    Wait-InputLog 'composition.*COMPSTR' $inputOffset
    if ((Read-InputLog).Substring($inputOffset) -match 'composition.*RESULTSTR') {
        throw 'The input method committed before the explicit confirmation.'
    }
    Wait-Value $editor ''
    $report.checks.nativePreedit = $true
    Send-Keys $windowHandle @(0x20)
    Wait-InputLog 'composition.*RESULTSTR' $inputOffset
    Wait-Value $editor $committed
    $report.checks.nativeCommit = $true

    $report.phase = 'workspace-transfer'
    Click-Control $standalone ([regex]::Unescape('\u5728\u5de5\u4f5c\u533a\u6253\u5f00'))
    $workspace = Wait-Window 'workspace'
    $windowHandle = Activate-Window $workspace
    $editor = Find-Control $workspace ([regex]::Unescape('Markdown \u6e90\u6587'))
    Wait-Value $editor $committed
    if (!$editor.Current.HasKeyboardFocus) { throw 'The transferred editor lost keyboard focus.' }
    $report.checks.workspaceTransfer = $true
    Send-Keys $windowHandle @(0x11,0x5a)
    Wait-Value $editor $original
    $report.checks.undo = $true
    Send-Keys $windowHandle @(0x11,0x59)
    Wait-Value $editor $committed
    $report.checks.redo = $true
    Send-Keys $windowHandle @(0x11,0x23)
    Send-Keys $windowHandle @(0x10,0x25)
    Send-Keys $windowHandle @(0x10,0x25)
    $text = $editor.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
    $selection = @($text.GetSelection())
    if ($selection.Count -ne 1 -or $selection[0].GetText(-1) -cne $chinese) {
        throw 'The transferred editor did not select the two committed characters.'
    }
    $report.checks.selection = $true
    Send-Keys $windowHandle @(0x11,0x5a)
    Wait-Value $editor $original
    $after = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash.ToLowerInvariant()
    $report.fixtureSha256After = $after
    $report.checks.diskUnchanged = $before -eq $after
    if (!$report.checks.diskUnchanged) { throw 'The fixture file changed on disk.' }
    $report.passed = $true
} catch {
    $report.error = $_.Exception.Message
} finally {
    try {
        if ($app -and !$app.HasExited) {
            [void]$app.CloseMainWindow()
            if (!$app.WaitForExit(8000)) { $app.Kill() }
        }
        if ($app) { $app.WaitForExit() }
        $report.nativeInputEvents = @((Read-InputLog) -split "`r?`n" |
            Where-Object { $_ -match 'composition.*(?:COMPSTR|RESULTSTR)|sending markup=' } |
            Select-Object -First 80)
    } catch {
        $report.passed = $false
        $cleanupError = $_.Exception.Message
        if ($report.error) { $report.error += "; cleanup: $cleanupError" }
        else { $report.error = "Cleanup or log capture failed: $cleanupError" }
    } finally {
        if ($capture) { $capture.Dispose() }
        $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $evidence 'results.json') -Encoding UTF8
    }
}
if (!$report.passed) { throw $report.error }
