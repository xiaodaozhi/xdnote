# Explorer and the system association picker on a disposable Windows desktop.
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
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
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

function Read-MdUserChoice {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.md\UserChoice')
    if ($key) {
        try { return [string]$key.GetValue('ProgId') }
        finally { $key.Dispose() }
    }
    return ''
}

function Select-DefaultInProperties($Item, [string]$Name, [string]$DisplayExe) {
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
    Press-Control $change
    $picker = Wait-Result 'the Windows app picker' {
        $window = Foreground
        if ($window -and $window.Current.NativeWindowHandle -ne $properties.Current.NativeWindowHandle -and
            $window.Current.ClassName -eq 'Shell_Flyout' -and $window.Current.Name -match '\.md files') { $window }
    }
    # The server's Shell_Flyout is drawn but exposes no accessible children.
    $app = Wait-Result "$Name or More apps in the Windows picker" {
        Read-PickerPoint $picker @($Name,$DisplayExe,'More apps')
    }
    if ($app.Label -eq 'More apps') {
        Press-PickerPoint $picker $app
        $app = Wait-Result "$Name in the expanded Windows picker" {
            Read-PickerPoint $picker @($Name,$DisplayExe)
        }
    }
    Press-PickerPoint $picker $app
    $pickerHandle = $picker.Current.NativeWindowHandle
    $confirm = Read-PickerPoint $picker @('Set default','Always','OK','0K')
    if ($confirm) {
        Press-PickerPoint $picker $confirm
    } else {
        if (!(Picker-IsForeground $picker)) { throw 'The picker lost its foreground' }
        [ExplorerAcceptance]::keybd_event(13,0,0,[UIntPtr]::Zero)
        [ExplorerAcceptance]::keybd_event(13,0,2,[UIntPtr]::Zero)
    }
    $null = Wait-Result 'the app picker to close after confirming' {
        if ([ExplorerAcceptance]::GetForegroundWindow().ToInt64() -ne $pickerHandle) { $true }
    }
    Press-Control (Wait-Result 'the Properties confirmation button' { Named-Control $properties 'OK' })
}

$result = [ordered]@{ok=$false; buildIdentity=$metadata.buildIdentity; installerSha256=$identity.sha256;
    scope='Explorer item verbs and the visible Windows default application picker in a disposable desktop'}
$installed = $false
try {
    $originalChoice = Read-MdUserChoice
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/LANG=english','/TASKS=markdown',('/DIR="' + $root + '"'))
    $setup = Start-Process -FilePath $Package -ArgumentList $arguments -Wait -PassThru
    if ($setup.ExitCode -ne 0) { throw "Explorer fixture install failed: $($setup.ExitCode)" }
    $installed = $true
    $controlFile = $metadata.files | Where-Object { $_.path -eq 'xdnote.exe' }
    if (!$controlFile -or (Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant() -ne $controlFile.sha256) {
        throw 'Installed executable differs from the sealed payload'
    }
    $effectiveAfterInstall = [ExplorerAcceptance]::MarkdownHandler()
    # The preceding installer roundtrip enforces default-preservation. Explorer
    # can infer an executable from the new candidate list with no UserChoice.
    $result.installerPreservedExplicitChoice = ((Read-MdUserChoice) -eq $originalChoice)
    $result.inferredHandlerAfterInstall = ($effectiveAfterInstall -eq $exe)

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
    if ($effectiveAfterInstall -eq $exe) {
        Select-DefaultInProperties $cold 'Notepad' 'notepad.exe'
        $null = Wait-Result 'the selected fallback editor' {
            if ([IO.Path]::GetFileName([ExplorerAcceptance]::MarkdownHandler()) -eq 'notepad.exe') { $true }
        }
    }
    $beforeChoice = Read-MdUserChoice
    Select-DefaultInProperties $cold 'xdnote' 'xdnote.exe'
    $null = Wait-Result 'the selected default executable' {
        if ([ExplorerAcceptance]::MarkdownHandler() -eq $exe -and (Read-MdUserChoice) -ne $beforeChoice) { $true }
    }
    $result.selectedDefault = 'xdnote.exe'
    if (Owned-Processes) { throw 'An xdnote process was already running before the Explorer cold launch' }

    $processIds = @()
    foreach ($item in @($cold,$warm)) {
        $item.InvokeVerb('open')
        $opened = Wait-Result 'the installed xdnote document window' {
            Open-DocumentProcess ([IO.Path]::GetFileName($item.Path))
        }
        $processIds += $opened.Id
    }
    if ($processIds[0] -ne $processIds[1]) { throw 'File Explorer warm opening did not reuse the installed process' }
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
        if ($installed) {
            $uninstall = Start-Process -FilePath (Join-Path $root 'unins000.exe') -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
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
