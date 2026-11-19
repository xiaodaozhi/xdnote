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
} catch {
    $window = Foreground
    if ($window) {
        Write-Host "Foreground: $($window.Current.Name)"
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
