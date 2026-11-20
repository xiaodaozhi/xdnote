$ErrorActionPreference = 'Stop'
$serial = [IO.Ports.SerialPort]::new('COM1', 115200, 'None', 8, 'One')
$serial.Open()
function Send-Record($name, $data) {
    $json = $data | ConvertTo-Json -Depth 12 -Compress
    $serial.WriteLine($name + ' ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json)))
}
function Stage($name) { $serial.WriteLine('XDNOTE_VM_STAGE ' + $name) }
function Wait-For($label, [scriptblock]$probe) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $value = & $probe
        if ($value) { return $value }
        Start-Sleep -Milliseconds 250
    } while ($clock.Elapsed.TotalSeconds -lt 35)
    throw "Timed out waiting for $label"
}
function Characters([int[]]$codes) { -join ($codes | ForEach-Object { [char]$_ }) }

Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Windows.Forms,System.IO.Compression.FileSystem
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CandidateShell {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("shlwapi.dll", CharSet=CharSet.Unicode)]
    static extern int AssocQueryString(int flags, int query, string association, string verb, StringBuilder value, ref uint size);
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(uint eventId, uint flags, IntPtr first, IntPtr second);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct ShellExecuteInfo {
        public int Size; public uint Mask; public IntPtr Owner;
        public string Verb, File, Parameters, Directory;
        public int Show; public IntPtr Instance, IdList;
        public string ClassName; public IntPtr KeyClass;
        public uint HotKey; public IntPtr Icon, Process;
    }
    [DllImport("shell32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool ShellExecuteEx(ref ShellExecuteInfo info);
    public static string Handler(string extension) {
        var value = new StringBuilder(1024);
        uint size = (uint)value.Capacity;
        return AssocQueryString(0, 2, extension, "open", value, ref size) == 0 ? value.ToString() : "";
    }
    public static void OpenWithXdnote(string file) {
        var request = new ShellExecuteInfo { Mask = 0x101, Verb = "open", File = file, Show = 1, ClassName = "xdnote.Markdown" };
        request.Size = Marshal.SizeOf(request);
        if (!ShellExecuteEx(ref request))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    }
}
'@

function Window([int]$owner, [string]$title) {
    $root = [Windows.Automation.AutomationElement]::RootElement
    $scope = [Windows.Automation.TreeScope]::Children
    $filter = [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ProcessIdProperty, $owner)
    $root.FindAll($scope, $filter) | Where-Object { $_.Current.Name -like "*$title*" -and !$_.Current.IsOffscreen } | Select-Object -First 1
}
function Control($window, [string]$id, [string]$label) {
    $descendants = [Windows.Automation.TreeScope]::Descendants
    $elements = $window.FindAll($descendants, [Windows.Automation.Condition]::TrueCondition)
    foreach ($element in $elements) {
        $automationId = [string]$element.Current.AutomationId
        $matches = ($automationId -eq $id -or $automationId.EndsWith(".$id") -or $element.Current.Name -eq $label)
        $invoke = $null
        if ($matches -and $element.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$invoke)) {
            return $element
        }
    }
}
function Invoke-Button($control) {
    $control.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Use-Window($window) {
    $handle = [IntPtr]$window.Current.NativeWindowHandle
    [void][CandidateShell]::SetForegroundWindow($handle)
    $window.SetFocus()
    [void](Wait-For 'xdnote foreground window' { [CandidateShell]::GetForegroundWindow() -eq $handle })
}
function Association-State {
    $state = [ordered]@{}
    foreach ($extension in '.md', '.markdown', '.txt') {
        $choice = Get-ItemProperty -LiteralPath "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$extension\UserChoice" -ErrorAction SilentlyContinue
        $state[$extension] = [ordered]@{ handler = [CandidateShell]::Handler($extension); userChoice = $choice.ProgId; userChoiceHash = $choice.Hash }
    }
    return $state
}
function Effective-App([string]$handler) {
    $handler -and [IO.Path]::GetFileName($handler) -notin 'OpenWith.exe', 'OpenWithHost.exe'
}

$result = [ordered]@{ok=$false; checks=[ordered]@{}}
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $session = (Get-Process -Id $PID).SessionId
    if ($os.ProductType -ne 1 -or $os.BuildNumber -notin '17763', '26100' -or $cpu.Architecture -ne 9 -or !$session) {
        throw 'Expected the interactive x64 client build 17763 or 26100'
    }
    $result.host = [ordered]@{ caption=$os.Caption; build=$os.BuildNumber; version=$os.Version; productType=$os.ProductType; architecture=$cpu.Architecture; session=$session }
    if ($os.BuildNumber -eq '26100') {
        $result.host.secureBoot = Confirm-SecureBootUEFI
        $result.host.tpmPresent = (Get-Tpm).TpmPresent
        if (!$result.host.secureBoot -or !$result.host.tpmPresent) { throw 'Windows 11 guest requires Secure Boot and TPM' }
    }
    Send-Record 'XDNOTE_VM_READY' $result.host
    $deadline = [DateTime]::UtcNow.AddMinutes(12)
    do {
        $vol = Get-CimInstance Win32_LogicalDisk | Where-Object VolumeName -eq 'XDNOTE_TESTS' | Select-Object -First 1
        if ($vol) { break }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$vol) { throw 'Read-only candidate disc did not arrive' }
    $root = 'C:\xdnote-candidate-acceptance'
    New-Item -ItemType Directory $root | Out-Null
    $expected = Get-Content -LiteralPath ($vol.DeviceID + '\identity.txt')
    if ($os.BuildNumber -ne (@{'1809'='17763'; '26100'='26100'}[$expected[2]])) { throw 'Wrong client for this sample' }
    $package = Join-Path $root 'candidate.msix'
    Copy-Item -LiteralPath ($vol.DeviceID + '\candidate.msix') -Destination $package
    $result.packageSha256 = (Get-FileHash -LiteralPath $package).Hash.ToLowerInvariant()
    if ($result.packageSha256 -ne $expected[0]) { throw 'Original package digest differs' }

    # Seed a recoverable default through ordinary per-user class registration.
    # The guest has no user data and protected UserChoice is never written.
    $classes = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Classes')
    $command = $classes.CreateSubKey('Xdnote.Acceptance.Notepad\shell\open\command')
    $command.SetValue('', ('"{0}\System32\notepad.exe" "%1"' -f $env:WINDIR))
    $command.Close()
    foreach ($extension in '.md', '.markdown') {
        $type = $classes.CreateSubKey($extension)
        if (!(Effective-App ([CandidateShell]::Handler($extension)))) { $type.SetValue('', 'Xdnote.Acceptance.Notepad') }
        $type.Close()
    }
    [CandidateShell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    $result.defaultBefore = Association-State
    foreach ($extension in '.md', '.markdown') {
        if (!(Effective-App ($result.defaultBefore[$extension].handler))) { throw "No actual baseline app for $extension" }
    }

    Stage 'attempting original unsigned package registration'
    if (Get-AppxPackage -Name 'xiaodaozhi.xdnote') { throw 'Expected a clean guest without xdnote registration' }
    try {
        Add-AppxPackage -Path $package -ErrorAction Stop
        $result.msixInstall = [ordered]@{registered=$true}
    } catch {
        $result.msixInstall = [ordered]@{registered=$false; code=$_.FullyQualifiedErrorId; message=$_.Exception.Message}
    }
    if (($result.defaultBefore | ConvertTo-Json -Compress) -ne ((Association-State) | ConvertTo-Json -Compress)) {
        throw 'The original MSIX registration attempt changed a default association'
    }
    $result.checks.defaultsUnchangedByRegistrationAttempt = $true
    if (Get-AppxPackage -Name 'xiaodaozhi.xdnote') { throw 'Unsigned package unexpectedly registered; preserve the result for installed-path follow-up' }

    Stage 'extract verified bytes'
    $payload = Join-Path $root 'payload'
    [IO.Compression.ZipFile]::ExtractToDirectory($package, $payload)
    $exe = Join-Path $payload 'xdnote.exe'
    $result.executableSha256 = (Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()
    $versionJson = Join-Path $root 'version.json'
    $ver = Start-Process -FilePath $exe -ArgumentList '--version-json' -WorkingDirectory $payload -RedirectStandardOutput $versionJson -PassThru -Wait
    $result.identity = Get-Content -Raw -LiteralPath $versionJson | ConvertFrom-Json
    if ($ver.ExitCode -ne 0 -or $result.identity.version -ne '0.1.0' -or $result.identity.build -ne 1 -or $result.identity.revision -ne $expected[1] -or $result.identity.dirty) {
        throw 'Extracted xdnote build identity differs'
    }
    $utf8 = [Text.UTF8Encoding]::new($false)
    $first = Join-Path $root 'Cold Open Test.md'
    $second = Join-Path $root 'Association Open Test.markdown'
    $firstBody = "# Candidate read marker`n"
    [IO.File]::WriteAllText($first, $firstBody, $utf8)
    [IO.File]::WriteAllText($second, "# Shell association marker`n", $utf8)
    $secondDigest = (Get-FileHash -LiteralPath $second).Hash
    Stage 'opening the first Markdown file in a cold launch'
    $app = Start-Process -FilePath $exe -WorkingDirectory $payload -ArgumentList ('-- "{0}"' -f $first) -PassThru
    $firstWindow = Wait-For 'first Markdown document window' { Window $app.Id 'Cold Open Test' }
    $result.checks.coldOpen = $true
    $editText = Characters @(0x7f16,0x8f91)
    Invoke-Button (Wait-For 'Edit control' { Control $firstWindow 'editMode' $editText })
    $editorLabel = 'Markdown ' + (Characters @(0x6e90,0x6587))
    $editor = Wait-For 'Markdown editor' {
        $available = $firstWindow.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
        foreach ($element in $available) {
            if ($element.Current.Name -eq $editorLabel -and !$element.Current.IsOffscreen) { return $element }
        }
    }
    $valuePattern = $editor.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern)
    if (!$valuePattern.Current.Value.Contains('Candidate read marker')) { throw 'Opened document bytes were not visible to the editor' }
    $result.checks.editorRead = $true
    $edited = $firstBody + "candidate-save-marker`n"
    $valuePattern.SetValue($edited)
    [void](Wait-For 'edited buffer' { $valuePattern.Current.Value.Contains('candidate-save-marker') })
    Stage 'saving Markdown through the application'
    Use-Window $firstWindow
    $editor.SetFocus()
    [Windows.Forms.SendKeys]::SendWait('^s')
    [void](Wait-For 'saved bytes' { [IO.File]::ReadAllText($first, $utf8) -eq $edited })
    $result.savedSha256 = (Get-FileHash -LiteralPath $first).Hash.ToLowerInvariant()
    $result.checks.savedBytes = $true

    Stage 'registering xdnote as a non-default Markdown option'
    Use-Window $firstWindow
    [Windows.Forms.SendKeys]::SendWait('^,')
    $settings = Wait-For 'xdnote settings' { Window $app.Id (Characters @(0x8bbe,0x7f6e)) }
    $registerLabel = 'Markdown ' + (Characters @(0x6253,0x5f00,0x65b9,0x5f0f))
    Invoke-Button (Wait-For 'Register this copy' { Control $settings 'settingsRegisterTypes' $registerLabel })
    $registry = 'Registry::HKEY_CURRENT_USER\Software\Classes'
    $entry = Wait-For 'xdnote Markdown shell command' {
        $value = Get-Item -LiteralPath "$registry\xdnote.Markdown\shell\open\command" -ErrorAction SilentlyContinue
        if ($value) { $value.GetValue('') }
    }
    if ($entry -ne ('"{0}" "%1"' -f $exe)) { throw 'The Markdown shell command points to a different binary' }
    foreach ($extension in '.md', '.markdown') {
        $key = Get-Item -LiteralPath "$registry\$extension\OpenWithProgids"
        if ($key.GetValueNames() -notcontains 'xdnote.Markdown') { throw "Missing open-with registration for $extension" }
    }
    $result.associationCommand = $entry
    $result.checks.markdownAssociation = $true
    $result.defaultAfter = Association-State
    if (($result.defaultBefore | ConvertTo-Json -Compress) -ne ($result.defaultAfter | ConvertTo-Json -Compress)) {
        throw 'xdnote changed a pre-existing default'
    }
    $result.checks.defaultAppPreserved = $true

    Stage 'opening a second Markdown file via the registered shell class'
    [CandidateShell]::OpenWithXdnote($second)
    [void](Wait-For 'second Markdown document in original process' { Window $app.Id 'Association Open Test' })
    if ((Get-FileHash -LiteralPath $second).Hash -ne $secondDigest) { throw 'Opening the second sample changed its contents' }
    $result.checks.shellOpenSameProcess = $true
    $result.scope = 'original unsigned MSIX registration attempt and extracted payload on the native client'
    $result.ok = $true
} catch {
    $result.error = $_.Exception.Message
    $result.errorLine = $_.InvocationInfo.ScriptLineNumber
    try {
        $observedWindow = if ($settings) { $settings } else { $firstWindow }
        if ($observedWindow) {
            $result.controls = @($observedWindow.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition) |
                Select-Object -First 100 | ForEach-Object {
                    [ordered]@{name=$_.Current.Name; id=$_.Current.AutomationId; type=$_.Current.ControlType.ProgrammaticName}
                })
        }
    } catch { $result.controlObservationError = $_.Exception.Message }
} finally {
    Send-Record 'XDNOTE_VM_RESULT' $result
    $serial.Close()
}
