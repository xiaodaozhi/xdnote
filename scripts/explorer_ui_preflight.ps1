$ErrorActionPreference = 'Stop'
if ($env:RUNNER_ENVIRONMENT -ne 'github-hosted') { throw 'Disposable runner only' }
$source = Get-Content (Join-Path $PSScriptRoot 'check_windows_file_manager.ps1') -Raw -Encoding UTF8
$begin = $source.IndexOf('Add-Type -AssemblyName')
$end = $source.IndexOf('$result = [ordered]')
. ([scriptblock]::Create($source.Substring($begin, $end-$begin)))
$folder = Join-Path $env:RUNNER_TEMP ('xdnote GUI probe ' + $env:GITHUB_RUN_ID)
New-Item -ItemType Directory -Path $folder | Out-Null
$file = Join-Path $folder 'xdnote Explorer preflight.md'
[IO.File]::WriteAllText($file, '# Preflight')
$notepad = Join-Path $env:SystemRoot 'System32\notepad.exe'
if (!(Test-Path -LiteralPath $notepad)) { throw 'Stock Notepad unavailable' }
$registration = 'HKCU:\Software\Classes\xdnote.Acceptance.Notepad\shell\open\command'
$candidates = 'HKCU:\Software\Classes\.md\OpenWithProgids'
New-Item -Path $registration -Force | Out-Null
Set-Item -Path $registration -Value ('"' + $notepad + '" "%1"')
New-Item -Path $candidates -Force | Out-Null
New-ItemProperty -Path $candidates -Name 'xdnote.Acceptance.Notepad' -PropertyType String -Value '' -Force | Out-Null
$shell = New-Object -ComObject Shell.Application
try {
    $shell.Explore($folder)
    $explorer = Wait-Result 'the owned File Explorer folder' {
        foreach ($candidate in $shell.Windows()) {
            if ($candidate.LocationURL -and ([Uri]$candidate.LocationURL).LocalPath.TrimEnd('\') -eq $folder) {
                $candidate
                break
            }
        }
    }
    Write-Host 'Explorer opened the disposable folder'
    $item = $explorer.Document.Folder.ParseName([IO.Path]::GetFileName($file))
    if ($item.Path -ne $file) { throw 'Missing owned item' }
    Select-DefaultInProperties $item 'Notepad' 'notepad.exe'
    $handler = [ExplorerAcceptance]::MarkdownHandler()
    if ([IO.Path]::GetFileName($handler) -ne 'notepad.exe') { throw 'Notepad was not selected' }
    Write-Host 'Explorer Properties chose Notepad through the Windows picker'
    $item.InvokeVerb('open')
    $open = Wait-Result 'the owned file in Notepad' {
        Get-Process notepad -ErrorAction SilentlyContinue | Where-Object {
            $_.MainWindowTitle -like '*xdnote Explorer preflight*'
        } | Select-Object -First 1
    }
    if (!$open.CloseMainWindow()) { throw 'Could not close the owned Notepad window' }
    Write-Host 'Explorer used the selected handler and showed the owned document'
    $wordpad = Join-Path $env:ProgramFiles 'Windows NT\Accessories\wordpad.exe'
    if (Test-Path -LiteralPath $wordpad) {
        $alternate = 'HKCU:\Software\Classes\xdnote.Acceptance.WordPad\shell\open\command'
        New-Item -Path $alternate -Force | Out-Null
        Set-Item -Path $alternate -Value ('"' + $wordpad + '" "%1"')
        New-ItemProperty -Path $candidates -Name 'xdnote.Acceptance.WordPad' -PropertyType String -Value '' -Force | Out-Null
        [ExplorerAcceptance]::SHChangeNotify(0x08000000,0,[IntPtr]::Zero,[IntPtr]::Zero)
        $choice = Read-MdUserChoice
        Select-DefaultInProperties $item 'WordPad' 'wordpad.exe'
        if ([IO.Path]::GetFileName([ExplorerAcceptance]::MarkdownHandler()) -ne 'wordpad.exe' -or (Read-MdUserChoice) -eq $choice) {
            throw 'Second explicit default was not selected'
        }
        Write-Host 'Explorer Properties selected a second explicit default through the picker'
        $choice = Read-MdUserChoice
        Select-DefaultInProperties $item 'UnlistedStockFixture' 'unlisted.exe' $notepad
        if ([IO.Path]::GetFileName([ExplorerAcceptance]::MarkdownHandler()) -ne 'notepad.exe' -or (Read-MdUserChoice) -eq $choice) {
            throw 'Browse did not select the third explicit default'
        }
        Write-Host 'Explorer Properties browsed to the Notepad executable and set it as the default'

    }
} catch {
    $window = Foreground
    if ($window) {
        Write-Host "Foreground: $($window.Current.Name)"
        Write-Host "Class: $($window.Current.ClassName), PID: $($window.Current.ProcessId), bounds: $($window.Current.BoundingRectangle)"
        Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class WindowProbe {
    private delegate bool Callback(IntPtr hwnd, IntPtr context);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr window, Callback callback, IntPtr context);
    public static IntPtr[] Children(IntPtr window) {
        var result = new List<IntPtr>();
        EnumChildWindows(window, (child, context) => { result.Add(child); return true; }, IntPtr.Zero);
        return result.ToArray();
    }
}
'@
        foreach ($child in [WindowProbe]::Children([ExplorerAcceptance]::GetForegroundWindow())) {
            $item = [Windows.Automation.AutomationElement]::FromHandle($child)
            if ($item) { Write-Host "Child HWND: $($item.Current.ClassName) | $($item.Current.Name)" }
        }
        $walker = [Windows.Automation.TreeWalker]::RawViewWalker
        for ($item = $walker.GetFirstChild($window); $item; $item = $walker.GetNextSibling($item)) {
            Write-Host "Raw UIA: $($item.Current.ClassName) | $($item.Current.Name)"
        }
        try {
            Add-Type -AssemblyName System.Runtime.WindowsRuntime
            $engine = [Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]::TryCreateFromUserProfileLanguages()
            if ($engine) { Write-Host "Windows OCR: $($engine.RecognizerLanguage.LanguageTag)" }
            else { Write-Host 'Windows OCR: no installed language' }
        } catch { Write-Host "Windows OCR unavailable: $($_.Exception.Message)" }
        if ($window.Current.Name -match 'open .md files|open this type of file') {
            try {
                Add-Type -AssemblyName System.Drawing
                $r = $window.Current.BoundingRectangle
                $width = [int]$r.Width
                $height = [int]$r.Height
                if ($width -gt 0 -and $height -gt 0 -and $width -le 1000 -and $height -le 900) {
                    New-Item -ItemType Directory -Path 'build/preflight' -Force | Out-Null
                    $bitmap = [Drawing.Bitmap]::new($width,$height)
                    $canvas = [Drawing.Graphics]::FromImage($bitmap)
                    try {
                        $canvas.CopyFromScreen([int]$r.X,[int]$r.Y,0,0,[Drawing.Size]::new($width,$height))
                        $bitmap.Save((Join-Path $PWD 'build/preflight/selector.png'),[Drawing.Imaging.ImageFormat]::Png)
                    } finally { $canvas.Dispose(); $bitmap.Dispose() }
                }
            } catch { Write-Host "Picker capture error: $($_.Exception.Message)" }
        }
        foreach ($control in $window.FindAll([Windows.Automation.TreeScope]::Descendants,
            [Windows.Automation.Condition]::TrueCondition) | Select-Object -First 100) {
            if ($control.Current.Name) {
                Write-Host "$($control.Current.ControlType.ProgrammaticName) $($control.Current.Name)"
            }
        }
    }
    foreach ($window in [Windows.Automation.AutomationElement]::RootElement.FindAll(
            [Windows.Automation.TreeScope]::Children, [Windows.Automation.Condition]::TrueCondition)) {
        if ($window.Current.Name -match 'xdnote|Properties|Open with|How do you want|app picker') {
            Write-Host "Window: $($window.Current.Name)"
        }
    }
    throw
}
