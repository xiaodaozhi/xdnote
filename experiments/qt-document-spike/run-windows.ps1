param(
    [Parameter(Mandatory = $true)][string]$QtPrefix,
    [string]$CMakePath = 'cmake',
    [string]$NinjaPath = 'ninja',
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$BuildName = 'qt-document-spike',
    [switch]$NativeWindowTests,
    [switch]$ManualHandoff,
    [switch]$Benchmarks,
    [ValidateRange(1, 5)][int]$BenchmarkRepeats = 1
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This entry point is for Windows; use the CMake instructions on other platforms.' }

$xdnoteRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$xdnoteQt = (Resolve-Path -LiteralPath $QtPrefix).Path
$xdnoteCmake = (Get-Command -Name $CMakePath -CommandType Application -ErrorAction Stop).Source
$xdnoteNinja = (Get-Command -Name $NinjaPath -CommandType Application -ErrorAction Stop).Source
$null = Get-Command -Name cl.exe -CommandType Application -ErrorAction Stop
if (!(Test-Path -LiteralPath (Join-Path $xdnoteQt 'lib/cmake/Qt6/Qt6Config.cmake'))) {
    throw 'QtPrefix must be a Qt SDK prefix containing Qt6Config.cmake.'
}

$xdnoteBuild = Join-Path $xdnoteRoot ("build/" + $BuildName)
$xdnoteTemp = Join-Path $xdnoteRoot 'build/temp'
$xdnoteRunId = [guid]::NewGuid().ToString('N')
$xdnoteRun = Join-Path $xdnoteBuild ("runs/" + $xdnoteRunId)
New-Item -ItemType Directory -Path $xdnoteBuild,$xdnoteTemp,$xdnoteRun -Force | Out-Null

$xdnoteEnvironmentKeys = @('PATH','TEMP','TMP','VSLANG','QT_PLUGIN_PATH','QML_IMPORT_PATH','QML2_IMPORT_PATH',
    'QT_QPA_PLATFORM','QT_QUICK_BACKEND','QT_FORCE_STDERR_LOGGING')
$xdnotePreviousEnvironment = @{}
foreach ($xdnoteKey in $xdnoteEnvironmentKeys) {
    $xdnotePreviousEnvironment[$xdnoteKey] = [Environment]::GetEnvironmentVariable($xdnoteKey, 'Process')
}

function Invoke-BuildStep {
    param([string]$Program, [string[]]$CommandArguments, [string]$LogPath)
    & $Program @CommandArguments *> $LogPath
    $xdnoteStatus = $LASTEXITCODE
    if ($xdnoteStatus -ne 0) {
        Get-Content -LiteralPath $LogPath | Select-String -Pattern 'FAILED:|fatal error|error C[0-9]+|error LNK[0-9]+|error:' | Select-Object -Last 20
        throw "Build step failed with exit code $xdnoteStatus. See $LogPath"
    }
}

function Invoke-SpikeTests {
    param([string]$Platform, [string]$ReportPath)
    $env:QT_QPA_PLATFORM = $Platform
    & (Join-Path $xdnoteBuild 'qt_document_spike_tests.exe') -o "$ReportPath,txt"
    $xdnoteStatus = $LASTEXITCODE
    if (Test-Path -LiteralPath $ReportPath) { Get-Content -LiteralPath $ReportPath -Raw }
    if ($xdnoteStatus -ne 0) { throw "Qt tests on $Platform failed with exit code $xdnoteStatus. See $ReportPath" }
    if (!(Test-Path -LiteralPath $ReportPath)) { throw "Qt tests did not create a report: $ReportPath" }
}

function Invoke-HandoffSmoke {
    param([string]$ReportPath)
    $env:QT_QPA_PLATFORM = 'offscreen'
    & (Join-Path $xdnoteBuild 'qt_document_handoff_manual.exe') --smoke-test *> $ReportPath
    $xdnoteStatus = $LASTEXITCODE
    if (Test-Path -LiteralPath $ReportPath) { Get-Content -LiteralPath $ReportPath }
    if ($xdnoteStatus -ne 0) { throw "Manual handoff smoke test failed with exit code $xdnoteStatus. See $ReportPath" }
    Write-Output 'Manual handoff offscreen smoke test: passed.'
}

function Invoke-WidgetAccessibilityTests {
    param([string]$Platform, [string]$ReportPath)
    $env:QT_QPA_PLATFORM = $Platform
    & (Join-Path $xdnoteBuild 'qt_widget_accessibility_tests.exe') -o "$ReportPath,txt"
    $xdnoteStatus = $LASTEXITCODE
    if (Test-Path -LiteralPath $ReportPath) { Get-Content -LiteralPath $ReportPath -Raw }
    if ($xdnoteStatus -ne 0) { throw "Widget accessibility tests on $Platform failed with exit code $xdnoteStatus. See $ReportPath" }
    if (!(Test-Path -LiteralPath $ReportPath)) { throw "Widget accessibility tests did not create a report: $ReportPath" }
}

function Get-SourceFingerprint {
    $xdnoteRelativePaths = @('CMakeLists.txt', 'run-windows.ps1', 'src/spike_document.h',
        'src/spike_document.cpp', 'src/spike_handoff.h', 'src/spike_handoff.cpp',
        'src/spike_ipc.h', 'src/spike_ipc.cpp', 'manual/handoff.cpp', 'manual/handoff.qml',
        'ipc/send_open.cpp', 'tests/file_identity_probe.h', 'tests/file_identity_probe.cpp',
        'tests/image_reference_probe.h', 'tests/image_reference_probe.cpp',
        'tests/windows_junction_probe.h', 'tests/windows_junction_probe.cpp',
        'tests/windows_commit_diagnostic.h', 'tests/windows_commit_diagnostic.cpp',
        'tests/fixture_writer.cpp', 'tests/uia_client.cpp', 'tests/displaced_probe.h', 'tests/displaced_probe.cpp',
        'tests/qt_spike_test.cpp', 'tests/widget_accessibility_test.cpp',
        'tests/fixtures/markdown-core.md', 'benchmark/benchmark.cpp')
    $xdnoteEntries = foreach ($xdnoteRelativePath in $xdnoteRelativePaths) {
        $xdnoteSourceFile = Join-Path $PSScriptRoot $xdnoteRelativePath
        [ordered]@{
            path = $xdnoteRelativePath
            bytes = (Get-Item -LiteralPath $xdnoteSourceFile).Length
            sha256 = (Get-FileHash -LiteralPath $xdnoteSourceFile -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    return ConvertTo-Json -InputObject @($xdnoteEntries) -Depth 4 -Compress
}

function Invoke-Benchmark {
    param([int]$Bytes, [string]$ReportPath)
    $env:QT_QPA_PLATFORM = 'windows'
    $xdnoteInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $xdnoteInfo.FileName = Join-Path $xdnoteBuild 'qt_document_spike_benchmark.exe'
    $xdnoteInfo.UseShellExecute = $false
    $xdnoteInfo.CreateNoWindow = $true
    $xdnoteInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    foreach ($xdnoteArgument in @('--size', [string]$Bytes, '--output', $ReportPath)) {
        [void]$xdnoteInfo.ArgumentList.Add($xdnoteArgument)
    }
    $xdnoteProcess = [System.Diagnostics.Process]::Start($xdnoteInfo)
    if (!$xdnoteProcess) { throw 'Could not start benchmark process.' }
    try {
        if (!$xdnoteProcess.WaitForExit(30000)) {
            $xdnoteProcess.Kill()
            $xdnoteProcess.WaitForExit()
            [ordered]@{ status = 'timeout'; sampleBytes = $Bytes; limitMs = 30000 } |
                ConvertTo-Json | Set-Content -LiteralPath $ReportPath -Encoding utf8NoBOM
            Write-Output "Benchmark $Bytes bytes: timed out after 30 seconds."
            return
        }
        if ($xdnoteProcess.ExitCode -ne 0) {
            throw "Benchmark $Bytes bytes failed with exit code $($xdnoteProcess.ExitCode)."
        }
        if (!(Test-Path -LiteralPath $ReportPath)) { throw "Benchmark did not create $ReportPath" }
        $xdnoteData = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
        Write-Output ("Benchmark {0} bytes: open {1:N1} ms, parse {2:N1} ms, layout {3:N1} ms" -f
            $Bytes, $xdnoteData.timingMs.fileOpenAndDocumentLoad,
            $xdnoteData.timingMs.markdownParse, $xdnoteData.timingMs.previewLayout)
    } finally {
        $xdnoteProcess.Dispose()
    }
}

try {
    $env:TEMP = $xdnoteTemp
    $env:TMP = $xdnoteTemp
    $env:VSLANG = '1033'
    $env:PATH = (Join-Path $xdnoteQt 'bin') + ';' + $xdnotePreviousEnvironment['PATH']
    $env:QT_PLUGIN_PATH = Join-Path $xdnoteQt 'plugins'
    $env:QML_IMPORT_PATH = Join-Path $xdnoteQt 'qml'
    $env:QML2_IMPORT_PATH = $null
    $env:QT_QUICK_BACKEND = 'software'
    $env:QT_FORCE_STDERR_LOGGING = '1'

    $xdnoteSource = Join-Path $xdnoteRoot 'experiments/qt-document-spike'
    $xdnoteFingerprint = Get-SourceFingerprint
    Set-Content -LiteralPath (Join-Path $xdnoteRun 'source-fingerprint.json') -Value $xdnoteFingerprint -Encoding utf8NoBOM
    $xdnoteConfigureArgs = @('-S', $xdnoteSource, '-B', $xdnoteBuild, '-G', 'Ninja',
        "-DCMAKE_PREFIX_PATH=$xdnoteQt", "-DCMAKE_MAKE_PROGRAM=$xdnoteNinja",
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TRY_COMPILE_CONFIGURATION=Release')
    Invoke-BuildStep -Program $xdnoteCmake -CommandArguments $xdnoteConfigureArgs -LogPath (Join-Path $xdnoteRun 'configure.log')
    Invoke-BuildStep -Program $xdnoteCmake -CommandArguments @('--build', $xdnoteBuild) -LogPath (Join-Path $xdnoteRun 'build.log')
    Invoke-SpikeTests -Platform 'offscreen' -ReportPath (Join-Path $xdnoteRun 'offscreen.txt')
    Invoke-WidgetAccessibilityTests -Platform 'offscreen' -ReportPath (Join-Path $xdnoteRun 'widget-accessibility-offscreen.txt')
    Invoke-HandoffSmoke -ReportPath (Join-Path $xdnoteRun 'handoff-smoke.txt')
    if ($NativeWindowTests) {
        Invoke-SpikeTests -Platform 'windows' -ReportPath (Join-Path $xdnoteRun 'windows.txt')
        Invoke-WidgetAccessibilityTests -Platform 'windows' -ReportPath (Join-Path $xdnoteRun 'widget-accessibility-windows.txt')
    }
    if ($Benchmarks) {
        foreach ($xdnoteBytes in @(10240, 1048576, 10485760)) {
            for ($xdnoteRepeat = 1; $xdnoteRepeat -le $BenchmarkRepeats; $xdnoteRepeat++) {
                $xdnoteReportName = "benchmark-$xdnoteBytes-run$xdnoteRepeat.json"
                Invoke-Benchmark -Bytes $xdnoteBytes -ReportPath (Join-Path $xdnoteRun $xdnoteReportName)
            }
        }
    }
    if ($ManualHandoff) {
        $env:QT_QPA_PLATFORM = 'windows'
        & (Join-Path $xdnoteBuild 'qt_document_handoff_manual.exe')
        if ($LASTEXITCODE -ne 0) { throw "Manual handoff experiment exited with code $LASTEXITCODE." }
    }
    if ((Get-SourceFingerprint) -cne $xdnoteFingerprint) {
        throw 'Experiment sources changed during the run; results are not bound to a stable source snapshot.'
    }
    Write-Output "Run evidence: $xdnoteRun"
} finally {
    foreach ($xdnoteKey in $xdnoteEnvironmentKeys) {
        [Environment]::SetEnvironmentVariable($xdnoteKey, $xdnotePreviousEnvironment[$xdnoteKey], 'Process')
    }
}
