# Visible setup, Explorer and the association picker on a disposable Windows desktop.
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$Audit,
    [Parameter(Mandatory=$true)][string]$PackageReport,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or
    $env:GITHUB_RUN_ID -notmatch '^\d+$' -or (Get-Process -Id $PID).SessionId -eq 0) {
    throw 'An interactive disposable GitHub-hosted Windows runner is required'
}

$build = [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) 'build')) + '\'
$output = [IO.Path]::GetFullPath($EvidenceDirectory)
if (!$output.StartsWith($build, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)) {
    throw 'Use a new evidence directory in build/'
}
$metadata = Get-Content -LiteralPath $Audit -Raw | ConvertFrom-Json
$identity = Get-Content -LiteralPath $PackageReport -Raw | ConvertFrom-Json
if (!$metadata.ok -or !$identity.ok -or $identity.dirty -or $identity.qaFixture -or
    $metadata.buildIdentity.buildVersion.revision -ne $env:GITHUB_SHA -or
    (Get-FileHash -LiteralPath $Package).Hash.ToLowerInvariant() -ne $identity.sha256) {
    throw 'An original, clean, sealed product installer is required'
}
$root = Join-Path $env:RUNNER_TEMP ('xdnote Explorer application, ' + $env:GITHUB_RUN_ID)
$files = Join-Path $env:RUNNER_TEMP ('xdnote Explorer samples, ' + $env:GITHUB_RUN_ID)
if ((Test-Path -LiteralPath $root) -or (Test-Path -LiteralPath $files)) {
    throw 'Existing acceptance data was preserved'
}
New-Item -ItemType Directory -Path $output,$files | Out-Null
$exe = Join-Path $root 'xdnote.exe'
# Windows PowerShell 5.1 loads this source in the system code page.
$coldLabel = -join [char[]]@(0x51B7,0x542F,0x52A8)
$warmLabel = -join [char[]]@(0x8FD0,0x884C,0x4E2D,0x8F6C,0x4EA4)
$samples = @("xdnote Explorer $coldLabel.md", "xdnote Explorer $warmLabel.md")
$hashes = @{}
foreach ($name in $samples) {
    $path = Join-Path $files $name
    [IO.File]::WriteAllText($path, "# $name`n")
    $hashes[$name] = (Get-FileHash -LiteralPath $path).Hash
}

Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing,System.Runtime.WindowsRuntime
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ExplorerAcceptance {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr parameter, string value);
    public static bool SetBrowseFilename(IntPtr dialog, string path) {
        var field = FindWindowEx(dialog, IntPtr.Zero, "ComboBoxEx32", null);
        if (field == IntPtr.Zero) return false;
        var combo = FindWindowEx(field, IntPtr.Zero, "ComboBox", null);
        if (combo == IntPtr.Zero) return false;
        var edit = FindWindowEx(combo, IntPtr.Zero, "Edit", null);
        return edit != IntPtr.Zero && SendMessage(edit, 0x000C, IntPtr.Zero, path) != IntPtr.Zero;
    }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
    public static void ScrollDown() { mouse_event(0x0800, 0, 0, unchecked((uint)-720), UIntPtr.Zero); }
    [DllImport("Shlwapi.dll", CharSet = CharSet.Unicode)]
    static extern int AssocQueryString(uint flags, uint kind, string extension, string verb, StringBuilder path, ref uint size);
    public static string MarkdownHandler() {
        uint length = 32768;
        var path = new StringBuilder((int)length);
        return AssocQueryString(0, 2, ".md", "open", path, ref length) == 0 ? path.ToString() : "";
    }
}
'@

function Wait-Result([string]$Label, [scriptblock]$Probe) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 20) {
        $value = & $Probe
        if ($value) { return $value }
        Start-Sleep -Milliseconds 200
    }
    throw "Timed out waiting for $Label"
}

function Foreground {
    $handle = [ExplorerAcceptance]::GetForegroundWindow()
    if ($handle -ne [IntPtr]::Zero) {
        [Windows.Automation.AutomationElement]::FromHandle($handle)
    }
}

function Named-Control($Window, [string]$Name) {
    if ($Window) {
        $condition = [Windows.Automation.PropertyCondition]::new(
            [Windows.Automation.AutomationElement]::NameProperty, $Name,
            [Windows.Automation.PropertyConditionFlags]::IgnoreCase)
        $Window.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
    }
}

function Press-Control($Control) {
    $pattern = $null
    if ($Control.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) {
        $pattern.Select()
        return
    }
    if ($Control.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
        $pattern.Invoke()
        return
    }
    Click-Control $Control
}

function Click-Control($Control) {
    $rect = $Control.Current.BoundingRectangle
    if ($Control.Current.IsOffscreen -or $rect.IsEmpty) { throw 'Picker item is not visible' }
    if (![ExplorerAcceptance]::SetCursorPos([int]($rect.X + $rect.Width/2), [int]($rect.Y + $rect.Height/2))) {
        throw 'Could not target the picker item'
    }
    [ExplorerAcceptance]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
    [ExplorerAcceptance]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
}

function Wait-WinRT($Operation, [Type]$ResultType) {
    $method = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and
        $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    } | Select-Object -First 1
    $task = $method.MakeGenericMethod($ResultType).Invoke($null,@($Operation))
    if (!$task.Wait(5000)) { throw 'Windows picker OCR operation timed out' }
    return $task.Result
}

function Picker-IsForeground($Picker) {
    $front = Foreground
    return $front -and $front.Current.NativeWindowHandle -eq $Picker.Current.NativeWindowHandle
}

function Read-PickerPoint($Picker, [string[]]$Labels) {
    if (!(Picker-IsForeground $Picker)) { return }
    $rect = $Picker.Current.BoundingRectangle
    if ($rect.Width -le 0 -or $rect.Width -gt 1000 -or $rect.Height -le 0 -or $rect.Height -gt 900) {
        throw 'Unexpected system picker bounds'
    }
    $size = [Drawing.Size]::new([int]$rect.Width, [int]$rect.Height)
    $path = [IO.Path]::GetTempFileName()
    $bitmap = [Drawing.Bitmap]::new($size.Width, $size.Height)
    $canvas = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $canvas.CopyFromScreen([int]$rect.X,[int]$rect.Y,0,0,$size)
        $bitmap.Save($path,[Drawing.Imaging.ImageFormat]::Png)
        $fileType = [Windows.Storage.StorageFile,Windows.Storage,ContentType=WindowsRuntime]
        $file = Wait-WinRT ($fileType::GetFileFromPathAsync($path)) $fileType
        $mode = [Windows.Storage.FileAccessMode,Windows.Storage,ContentType=WindowsRuntime]::Read
        $stream = Wait-WinRT ($file.OpenAsync($mode)) ([Windows.Storage.Streams.IRandomAccessStream,Windows.Storage.Streams,ContentType=WindowsRuntime])
        try {
            $decoderType = [Windows.Graphics.Imaging.BitmapDecoder,Windows.Foundation,ContentType=WindowsRuntime]
            $decoder = Wait-WinRT ($decoderType::CreateAsync($stream)) $decoderType
            $format = [Windows.Graphics.Imaging.BitmapPixelFormat,Windows.Foundation,ContentType=WindowsRuntime]::Bgra8
            $alpha = [Windows.Graphics.Imaging.BitmapAlphaMode,Windows.Foundation,ContentType=WindowsRuntime]::Ignore
            $image = Wait-WinRT ($decoder.GetSoftwareBitmapAsync($format,$alpha)) ([Windows.Graphics.Imaging.SoftwareBitmap,Windows.Foundation,ContentType=WindowsRuntime])
            $ocr = [Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]::TryCreateFromUserProfileLanguages()
            if (!$ocr) { throw 'No Windows OCR language is installed' }
            $text = Wait-WinRT ($ocr.RecognizeAsync($image)) ([Windows.Media.Ocr.OcrResult,Windows.Foundation,ContentType=WindowsRuntime])
            foreach ($label in $Labels) {
                foreach ($line in $text.Lines) {
                    $words = @($line.Words)
                    $count = @($label -split ' ').Count
                    for ($i=0; $i -le $words.Count-$count; $i++) {
                        $last = $i+$count-1
                        if (($words[$i..$last].Text -join ' ').Trim() -eq $label) {
                            $bounds = $words[$i].BoundingRect
                            return [pscustomobject]@{X=[int]($rect.X+$bounds.X+$bounds.Width/2);
                                Y=[int]($rect.Y+$bounds.Y+$bounds.Height/2); Label=$label}
                        }
                    }
                }
            }
        } finally { $stream.Dispose() }
    } finally {
        $canvas.Dispose()
        $bitmap.Dispose()
        Remove-Item -LiteralPath $path
    }
}

function Press-PickerPoint($Picker, $Point) {
    if (!(Picker-IsForeground $Picker) -or ![ExplorerAcceptance]::SetCursorPos($Point.X,$Point.Y)) {
        throw 'The system picker lost the foreground before selection'
    }
    [ExplorerAcceptance]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
    [ExplorerAcceptance]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
}

function Visible-SetupPage($Wizard, [string]$Title) {
    if (!(Picker-IsForeground $Wizard)) { return }
    $heading = Named-Control $Wizard $Title
    if ($heading -and !$heading.Current.IsOffscreen) { return $true }
    if (Read-PickerPoint $Wizard @($Title)) { return $true }
}

function Press-SetupAccelerator($Wizard, [byte]$Key) {
    if (!(Picker-IsForeground $Wizard)) { throw 'The setup wizard lost the foreground' }
    [ExplorerAcceptance]::keybd_event(0x12,0,0,[UIntPtr]::Zero)
    [ExplorerAcceptance]::keybd_event($Key,0,0,[UIntPtr]::Zero)
    [ExplorerAcceptance]::keybd_event($Key,0,2,[UIntPtr]::Zero)
    [ExplorerAcceptance]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
}

function Select-SetupMarkdownTask($Wizard) {
    $taskLabel = 'Offer xdnote in Open with (does not change your default app)'
    $task = Named-Control $Wizard $taskLabel
    $toggle = $null
    if ($task -and !$task.Current.IsOffscreen -and (Picker-IsForeground $Wizard) -and
        $task.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$toggle)) {
        if ($toggle.Current.ToggleState -ne [Windows.Automation.ToggleState]::Off) { throw 'The Markdown candidate was selected by default' }
        $toggle.Toggle()
        if ($toggle.Current.ToggleState -ne [Windows.Automation.ToggleState]::On) { throw 'The Markdown candidate was not selected' }
        return
    }
    if ($task -and !$task.Current.IsOffscreen -and (Picker-IsForeground $Wizard)) {
        Click-Control $task
        return
    }
    $point = Wait-Result 'the visible Markdown candidate' { Read-PickerPoint $Wizard @('Offer xdnote in Open with') }
    Press-PickerPoint $Wizard $point
    # A changed registry candidate after installation confirms the option switched on.
}

function Install-ThroughWizard($Result) {
    $arguments = @('/NORESTART','/LANG=english',('/DIR="' + $root + '"'),
        ('/LOG="' + (Join-Path $output 'setup.log') + '"'))
    $before = Foreground
    if ($before -and $before.Current.Name -eq 'Search' -and
        (Get-Process -Id $before.Current.ProcessId -ErrorAction SilentlyContinue).ProcessName -eq 'SearchHost') {
        [ExplorerAcceptance]::keybd_event(27,0,0,[UIntPtr]::Zero)
        [ExplorerAcceptance]::keybd_event(27,0,2,[UIntPtr]::Zero)
    }
    $setup = Start-Process -FilePath $Package -ArgumentList $arguments -PassThru
    try {
        $activator = New-Object -ComObject WScript.Shell
        $wizardClass = [Windows.Automation.PropertyCondition]::new(
            [Windows.Automation.AutomationElement]::ClassNameProperty, 'TWizardForm')
        $wizard = Wait-Result 'the visible xdnote Setup Wizard' {
            if ($setup.HasExited) { throw "Setup exited before displaying the wizard ($($setup.ExitCode))" }
            $windows = [Windows.Automation.AutomationElement]::RootElement.FindAll(
                [Windows.Automation.TreeScope]::Children, $wizardClass)
            foreach ($window in $windows) {
                if ($window.Current.Name -ne 'Setup - xdnote' -or $window.Current.IsOffscreen) { continue }
                $owner = $window.Current.ProcessId
                if ($owner -eq $setup.Id -or (Get-CimInstance Win32_Process -Filter "ProcessId = $owner").ParentProcessId -eq $setup.Id) {
                    $Result.setupWindowDiscovered = $true
                    if (Picker-IsForeground $window) { return $window }
                    $null = $activator.AppActivate($owner)
                }
            }
        }
        $Result.wizardPages = @()
        $pages = @('Welcome to the xdnote Setup Wizard', 'License Agreement', 'Information',
            'Select Destination Location', 'Select Additional Tasks')
        foreach ($page in $pages) {
            $null = Wait-Result "the setup page $page" { Visible-SetupPage $wizard $page }
            $Result.wizardPages += $page
            if ($page -eq 'License Agreement') {
                $notice = 'This agreement covers only the bundled Microsoft VC++ runtime. Other components retain their own licenses.'
                $null = Wait-Result 'the runtime license scope' { Visible-SetupPage $wizard $notice }
                $Result.wizardLicenseScope = $notice
                Press-SetupAccelerator $wizard 0x41 # Alt+A, Accept
            }
            if ($page -eq 'Select Additional Tasks') { Select-SetupMarkdownTask $wizard }
            Press-SetupAccelerator $wizard 0x4E # Alt+N, Next
        }
        $null = Wait-Result 'the ready-to-install page' { Visible-SetupPage $wizard 'Ready to Install' }
        $Result.wizardPages += 'Ready to Install'
        Press-SetupAccelerator $wizard 0x49 # Alt+I, Install
        $null = Wait-Result 'the visible installation completion page' { Visible-SetupPage $wizard 'Completing the xdnote Setup Wizard' }
        $Result.wizardPages += 'Completing the xdnote Setup Wizard'
        Press-SetupAccelerator $wizard 0x46 # Alt+F, Finish
        if (!$setup.WaitForExit(20000) -or $setup.ExitCode -ne 0) { throw 'The visible installer did not exit successfully' }
        $command = (Get-Item -LiteralPath 'HKCU:\Software\Classes\xdnote.Markdown\shell\open\command').GetValue('')
        if ($command -ne ('"' + $exe + '" -- "%1"')) { throw 'The wizard did not register the selected Markdown candidate' }
        $Result.wizardMarkdownCandidate = 'selected in the wizard'
    } catch {
        $Result.setupProcessExited = $setup.HasExited
        if ($setup.HasExited) { $Result.setupExitCode = $setup.ExitCode }
        $log = Join-Path $output 'setup.log'
        if (Test-Path -LiteralPath $log) { $Result.setupLogTail = @(Get-Content -LiteralPath $log -Tail 12) }
        if ($wizard -and (Picker-IsForeground $wizard)) {
            $Result.wizardControls = @($wizard.FindAll([Windows.Automation.TreeScope]::Descendants,
                [Windows.Automation.Condition]::TrueCondition) | Where-Object { !$_.Current.IsOffscreen } |
                Select-Object -First 60 | ForEach-Object { $_.Current.Name } | Where-Object { $_ })
        }
        throw
    } finally {
        # A failed diagnostic must release its private setup window before package cleanup.
        if (!$setup.HasExited) {
            $ownedChildren = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($setup.Id)"
            foreach ($child in $ownedChildren) { Stop-Process -Id $child.ProcessId -ErrorAction SilentlyContinue }
            if (!$setup.HasExited) { $setup.Kill(); $null = $setup.WaitForExit(3000) }
        }
    }
}

function Choose-Executable($Picker, $Browse, [string]$Path) {
    $owner = $Picker.Current.NativeWindowHandle
    Press-PickerPoint $Picker $Browse
    $dialog = Wait-Result 'the Windows executable browser' {
        $foreground = Foreground
        if ($foreground -and $foreground.Current.NativeWindowHandle -ne $owner -and
            $foreground.Current.ClassName -eq '#32770' -and $foreground.Current.Name -match 'Open|Browse') { $foreground }
    }
    $handle = [IntPtr]$dialog.Current.NativeWindowHandle
    if ([ExplorerAcceptance]::GetForegroundWindow() -ne $handle -or
        ![ExplorerAcceptance]::SetBrowseFilename($handle, $Path)) {
        throw 'Could not enter the installed path in the executable browser'
    }
    Click-Control (Wait-Result 'the executable browser Open button' { Named-Control $dialog 'Open' })
    $null = Wait-Result 'the executable browser to close' {
        if (![ExplorerAcceptance]::IsWindow($handle)) { $true }
    }
}

function Owned-Processes {
    Get-Process -Name xdnote -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
}

function Open-DocumentProcess([string]$Filename) {
    foreach ($process in @(Owned-Processes)) {
        $condition = [Windows.Automation.PropertyCondition]::new(
            [Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$process.Id)
        $windows = [Windows.Automation.AutomationElement]::RootElement.FindAll(
            [Windows.Automation.TreeScope]::Children, $condition)
        foreach ($window in $windows) {
            if ($window.Current.Name.Contains($Filename) -and !$window.Current.IsOffscreen) {
                return $process
            }
        }
    }
}

function Verify-ExplorerDocuments($Items) {
    if (Owned-Processes) { throw 'An xdnote process was already running before the Explorer cold launch' }
    $processIds = @()
    foreach ($item in $Items) {
        $item.InvokeVerb('open')
        $opened = Wait-Result 'the installed xdnote document window' {
            Open-DocumentProcess ([IO.Path]::GetFileName($item.Path))
        }
        $processIds += $opened.Id
    }
    if ($processIds[0] -ne $processIds[1]) { throw 'File Explorer warm opening did not reuse the installed process' }
    return $processIds[0]
}

function Read-MdUserChoice {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.md\UserChoice')
    if ($key) {
        try { return [string]$key.GetValue('ProgId') }
        finally { $key.Dispose() }
    }
    return ''
}

function Select-DefaultInProperties($Item, [string]$Name, [string]$DisplayExe, [string]$ExecutablePath='') {
    $stem = [IO.Path]::GetFileNameWithoutExtension($Item.Path)
    $Item.InvokeVerb('properties')
    $properties = Wait-Result 'the owned Markdown Properties dialog' {
        $window = Foreground
        if ($window -and $window.Current.Name.StartsWith($stem)) { $window }
    }
    $change = Wait-Result 'the Properties Change button' {
        $button = Named-Control $properties 'Change...'
        if (!$button) { $button = Named-Control $properties ('Change' + [char]0x2026) }
        $button
    }
    if ([ExplorerAcceptance]::GetForegroundWindow().ToInt64() -ne $properties.Current.NativeWindowHandle) {
        throw 'File Properties lost the foreground before Change'
    }
    Click-Control $change
    $observed = [Collections.Generic.List[string]]::new()
    $picker = Wait-Result 'the Windows app picker' {
        $window = Foreground
        if ($window) {
            $caption = $window.Current.ClassName + ': ' + $window.Current.Name
            if (!$observed.Contains($caption)) {
                $observed.Add($caption)
                Write-Host "Picker ($Name) foreground: $caption"
            }
            if ($window.Current.NativeWindowHandle -ne $properties.Current.NativeWindowHandle -and
                ($window.Current.ClassName -eq 'Open With' -or
                 ($window.Current.ClassName -eq 'Shell_Flyout' -and $window.Current.Name -match 'How do you want to open'))) {
                $window
            }
        }
    }
    # The server's Shell_Flyout is drawn but exposes no accessible children.
    $browseLabel = 'Look for another app on this PC'
    $app = Wait-Result "$Name or an alternate selection in the Windows picker" {
        Read-PickerPoint $picker @($Name,$DisplayExe,'More apps',$browseLabel)
    }
    if ($app.Label -eq 'More apps') {
        Press-PickerPoint $picker $app
        $app = Wait-Result "$Name or Browse in the expanded Windows picker" {
            $choice = Read-PickerPoint $picker @($Name,$DisplayExe,$browseLabel)
            if ($choice) { return $choice }
            if (Picker-IsForeground $picker) {
                $r = $picker.Current.BoundingRectangle
                $null = [ExplorerAcceptance]::SetCursorPos([int]($r.X+$r.Width*0.8),[int]($r.Y+$r.Height*0.6))
                [ExplorerAcceptance]::ScrollDown()
            }
        }
    }
    $pickerHandle = $picker.Current.NativeWindowHandle
    if ($app.Label -eq $browseLabel) {
        if (!$ExecutablePath -or !(Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) {
            throw 'No verified installed path for Browse'
        }
        Choose-Executable $picker $app $ExecutablePath
        Write-Host "Picker ($Name) received the verified executable path"
    } else {
        Press-PickerPoint $picker $app
    }
    if ([ExplorerAcceptance]::GetForegroundWindow().ToInt64() -eq $pickerHandle) {
        $confirm = Read-PickerPoint $picker @('Set default','Always','OK','0K')
        if ($confirm) { Press-PickerPoint $picker $confirm }
        else {
            if (!(Picker-IsForeground $picker)) { throw 'The picker lost its foreground' }
            [ExplorerAcceptance]::keybd_event(13,0,0,[UIntPtr]::Zero)
            [ExplorerAcceptance]::keybd_event(13,0,2,[UIntPtr]::Zero)
        }
    }
    $null = Wait-Result 'the app picker to close after confirming' {
        if ([ExplorerAcceptance]::GetForegroundWindow().ToInt64() -ne $pickerHandle) { $true }
    }
    $propertiesHandle = [IntPtr]$properties.Current.NativeWindowHandle
    Press-Control (Wait-Result 'the Properties confirmation button' { Named-Control $properties 'OK' })
    $null = Wait-Result 'the Markdown Properties dialog to close' {
        if (![ExplorerAcceptance]::IsWindow($propertiesHandle)) { $true }
    }
}

$result = [ordered]@{ok=$false; buildIdentity=$metadata.buildIdentity; installerSha256=$identity.sha256;
    scope='Visible setup, Explorer item verbs and the Windows default application picker in a disposable desktop'}
$uninstaller = Join-Path $root 'unins000.exe'
try {
    $originalChoice = Read-MdUserChoice
    Install-ThroughWizard $result
    $controlFile = $metadata.files | Where-Object { $_.path -eq 'xdnote.exe' }
    if (!$controlFile -or (Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant() -ne $controlFile.sha256) {
        throw 'Installed executable differs from the sealed payload'
    }
    $effectiveAfterInstall = [ExplorerAcceptance]::MarkdownHandler()
    # The preceding installer roundtrip enforces default-preservation. Explorer
    # can infer an executable from the new candidate list with no UserChoice.
    $result.installerPreservedExplicitChoice = ((Read-MdUserChoice) -eq $originalChoice)
    if (!$result.installerPreservedExplicitChoice) { throw 'The visible installer changed the explicit default app' }
    $result.inferredHandlerAfterInstall = ($effectiveAfterInstall -eq $exe)

    if ($effectiveAfterInstall -eq $exe) {
        # Add a stock alternative as a candidate only, before Explorer can cache
        # the associations. The chooser itself still makes each default choice.
        $notepad = Join-Path $env:SystemRoot 'System32\notepad.exe'
        if (!(Test-Path -LiteralPath $notepad)) { throw 'Stock Notepad unavailable' }
        $fallback = 'HKCU:\Software\Classes\xdnote.Acceptance.Notepad\shell\open\command'
        $candidates = 'HKCU:\Software\Classes\.md\OpenWithProgids'
        New-Item -Path $fallback -Force | Out-Null
        Set-Item -Path $fallback -Value ('"' + $notepad + '" "%1"')
        New-ItemProperty -Path $candidates -Name 'xdnote.Acceptance.Notepad' -PropertyType String -Value '' -Force | Out-Null
        [ExplorerAcceptance]::SHChangeNotify(0x08000000,0,[IntPtr]::Zero,[IntPtr]::Zero)
    }

    $shell = New-Object -ComObject Shell.Application
    $shell.Explore($files)
    $explorer = Wait-Result 'the owned File Explorer folder' {
        foreach ($candidate in $shell.Windows()) {
            if ($candidate.LocationURL -and ([Uri]$candidate.LocationURL).LocalPath.TrimEnd('\') -eq $files) {
                $candidate
                break
            }
        }
    }
    $folder = $explorer.Document.Folder
    $cold = $folder.ParseName($samples[0])
    $warm = $folder.ParseName($samples[1])
    if ($cold.Path -ne (Join-Path $files $samples[0]) -or $warm.Path -ne (Join-Path $files $samples[1])) {
        throw 'File Explorer did not resolve both owned document items'
    }
    if ([ExplorerAcceptance]::MarkdownHandler() -eq $exe) {
        $openedId = Verify-ExplorerDocuments @($cold,$warm)
        $result.inferredHandlerDocuments = $samples
        $result.inferredHandlerSameProcess = $true
        Stop-Process -Id $openedId
        Wait-Process -Id $openedId -Timeout 10 -ErrorAction SilentlyContinue
        $result.pickerStage = 'Notepad'
        Select-DefaultInProperties $cold 'Notepad' 'notepad.exe' $notepad
        $null = Wait-Result 'the selected fallback editor' {
            if ([IO.Path]::GetFileName([ExplorerAcceptance]::MarkdownHandler()) -eq 'notepad.exe') { $true }
        }
    }
    $beforeChoice = Read-MdUserChoice
    $result.pickerStage = 'xdnote'
    Select-DefaultInProperties $cold 'xdnote' 'xdnote.exe' $exe
    $null = Wait-Result 'the selected default executable' {
        if ([ExplorerAcceptance]::MarkdownHandler() -eq $exe -and (Read-MdUserChoice) -ne $beforeChoice) { $true }
    }
    $result.selectedDefault = 'xdnote.exe'
    $null = Verify-ExplorerDocuments @($cold,$warm)
    $result.documents = $samples
    $result.sameProcess = $true
    $result.ok = $true
} catch {
    $result.error = $_.Exception.Message
    $window = Foreground
    if ($window) {
        $result.foreground = $window.Current.Name
        $result.controls = @($window.FindAll([Windows.Automation.TreeScope]::Descendants,
            [Windows.Automation.Condition]::TrueCondition) | Select-Object -First 100 |
            ForEach-Object { $_.Current.Name } | Where-Object { $_ })
    }
    throw
} finally {
    foreach ($process in @(Owned-Processes)) { Stop-Process -Id $process.Id }
    try {
        if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
            $uninstall = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
            if ($uninstall.ExitCode -ne 0 -or (Test-Path -LiteralPath $exe)) { throw 'Explorer fixture uninstall failed' }
        }
        foreach ($name in $samples) {
            if ((Get-FileHash -LiteralPath (Join-Path $files $name)).Hash -ne $hashes[$name]) { throw 'An Explorer sample changed' }
        }
    } catch {
        $result.ok = $false
        $result.cleanupError = $_.Exception.Message
        throw
    } finally {
        $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
    }
}
