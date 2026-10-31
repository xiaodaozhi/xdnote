param(
    [Parameter(Mandatory = $true)][string]$QtPrefix,
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$BuildName = 'qt-document-spike',
    [ValidateSet('prepare','collect')][string]$Mode = 'prepare',
    [ValidateRange(1,200)][int]$MaxRuns = 200,
    [ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedExeSha256
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This diagnostic requires Windows.' }
$xdnoteRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$xdnoteBuildRoot = Join-Path $xdnoteRoot 'build'
$xdnoteBuild = Join-Path $xdnoteBuildRoot $BuildName
$xdnoteExecutable = Join-Path $xdnoteBuild 'qt_document_spike_tests.exe'
if (!(Test-Path -LiteralPath $xdnoteExecutable)) { $xdnoteExecutable = Join-Path $xdnoteBuild 'Release/qt_document_spike_tests.exe' }
$xdnoteExecutable = (Resolve-Path -LiteralPath $xdnoteExecutable).Path
if (!$xdnoteExecutable.StartsWith($xdnoteBuildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The test executable must remain inside the repository build directory.'
}
$xdnoteAncestor = Get-Item -LiteralPath $xdnoteExecutable
while ($xdnoteAncestor.FullName -ne $xdnoteRoot) {
    if ($xdnoteAncestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse points are not accepted in the diagnostic build path.' }
    $xdnoteAncestor = if ($xdnoteAncestor -is [IO.DirectoryInfo]) { $xdnoteAncestor.Parent } else { $xdnoteAncestor.Directory }
    if (!$xdnoteAncestor) { throw 'Diagnostic path left the repository.' }
}
$xdnoteQt = (Resolve-Path -LiteralPath $QtPrefix).Path
if (!(Test-Path -LiteralPath (Join-Path $xdnoteQt 'bin/Qt6Core.dll'))) { throw 'QtPrefix must identify the tested Qt SDK.' }
$xdnoteHash = (Get-FileHash -LiteralPath $xdnoteExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($Mode -eq 'collect' -and (!$ExpectedExeSha256 -or $ExpectedExeSha256.ToLowerInvariant() -cne $xdnoteHash)) {
    throw 'Collection requires the unchanged executable hash reported by prepare.'
}
$xdnoteId = [guid]::NewGuid().ToString('N')
$xdnoteRun = Join-Path $xdnoteBuildRoot ('issue26-kernel-' + $xdnoteId)
New-Item -ItemType Directory -Path $xdnoteRun -ErrorAction Stop | Out-Null
$xdnoteProbeName = 'xdnote_issue26_' + $xdnoteId + '.exe'
$xdnoteProbe = Join-Path $xdnoteRun $xdnoteProbeName
Copy-Item -LiteralPath $xdnoteExecutable -Destination $xdnoteProbe
$xdnoteProfile = Join-Path $xdnoteRun 'rename.wprp'
$xdnoteXml = @"
<?xml version="1.0" encoding="utf-8"?>
<WindowsPerformanceRecorder Version="1.0" Author="xdnote">
  <Profiles>
    <EventCollector Id="RenameCollector" Name="xdnote rename $xdnoteId">
      <BufferSize Value="64"/><Buffers Value="64"/>
    </EventCollector>
    <EventProvider Id="RenameProvider" Name="edd08927-9cc4-4e65-b970-c2560fb5c289" Level="5" Stack="true" ProcessExeFilter="$xdnoteProbeName">
      <Keywords><Keyword Value="0x860"/></Keywords>
      <EventFilters FilterIn="true"><EventId Value="17"/><EventId Value="19"/><EventId Value="24"/><EventId Value="27"/><EventId Value="29"/></EventFilters>
    </EventProvider>
    <Profile Id="XdnoteRename.Verbose.Memory" Name="XdnoteRename" Description="Only the dedicated xdnote fixture process rename events" LoggingMode="Memory" DetailLevel="Verbose">
      <Collectors><EventCollectorId Value="RenameCollector"><EventProviders><EventProviderId Value="RenameProvider"/></EventProviders></EventCollectorId></Collectors>
    </Profile>
  </Profiles>
</WindowsPerformanceRecorder>
"@
Set-Content -LiteralPath $xdnoteProfile -Value $xdnoteXml -Encoding utf8NoBOM
& wpr.exe -profiles $xdnoteProfile *> (Join-Path $xdnoteRun 'profile-validation.txt')
if ($LASTEXITCODE -ne 0) { throw "WPR rejected the constrained profile; see $xdnoteRun" }
$xdnotePrincipal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
$xdnoteAdmin = $xdnotePrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$xdnotePlan = [ordered]@{ executableSha256=$xdnoteHash; administrator=$xdnoteAdmin; mode=$Mode; runDirectory=$xdnoteRun; processFilter=$xdnoteProbeName; eventIds=@(17,19,24,27,29); maxRuns=$MaxRuns }
$xdnotePlan | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $xdnoteRun 'plan.json') -Encoding utf8NoBOM
if ($Mode -eq 'prepare') { $xdnotePlan | ConvertTo-Json -Compress; return }
if (!$xdnoteAdmin) { throw 'Kernel file stack collection requires an administrator token. No recording was started.' }

$xdnoteKeys = @('PATH','TEMP','TMP','QT_PLUGIN_PATH','QML_IMPORT_PATH','QT_QPA_PLATFORM','QT_QUICK_BACKEND','XDNOTE_TRACE_QSAVEFILE','XDNOTE_TRACE_FIXTURE_USERS')
$xdnotePrevious = @{}
foreach ($xdnoteKey in $xdnoteKeys) { $xdnotePrevious[$xdnoteKey]=[Environment]::GetEnvironmentVariable($xdnoteKey,'Process') }
$xdnoteRecording = $false
$xdnoteFirstFailure = 0
$xdnoteExit = 0
$xdnoteCollectionError = $null
try {
    $env:PATH = (Join-Path $xdnoteQt 'bin') + ';' + $env:PATH
    $env:TEMP = $xdnoteRun; $env:TMP = $xdnoteRun
    $env:QT_PLUGIN_PATH = Join-Path $xdnoteQt 'plugins'
    $env:QML_IMPORT_PATH = Join-Path $xdnoteQt 'qml'
    $env:QT_QPA_PLATFORM = 'offscreen'; $env:QT_QUICK_BACKEND = 'software'
    $env:XDNOTE_TRACE_QSAVEFILE = '1'; $env:XDNOTE_TRACE_FIXTURE_USERS = '0'
    & wpr.exe -start "${xdnoteProfile}!XdnoteRename.Verbose" -instancename $xdnoteId -recordtempto $xdnoteRun *> (Join-Path $xdnoteRun 'start.txt')
    if ($LASTEXITCODE -ne 0) { throw 'The dedicated WPR session did not start; no other session will be stopped.' }
    $xdnoteRecording = $true
    for ($xdnoteAttempt=1; $xdnoteAttempt -le $MaxRuns; $xdnoteAttempt++) {
        $xdnoteReport=Join-Path $xdnoteRun "run-$xdnoteAttempt.txt"
        $xdnoteInfo=[Diagnostics.ProcessStartInfo]::new()
        $xdnoteInfo.FileName=$xdnoteProbe
        $xdnoteInfo.UseShellExecute=$false
        $xdnoteInfo.CreateNoWindow=$true
        $xdnoteInfo.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
        foreach ($xdnoteArgument in @('changeAfterFinalCheckCanBeOverwritten','editDuringSaveStaysDirty','nonBreakingSpaceIsNotRewritten','-o',"${xdnoteReport},txt")) { [void]$xdnoteInfo.ArgumentList.Add($xdnoteArgument) }
        $xdnoteProcess=[Diagnostics.Process]::Start($xdnoteInfo)
        try {
            if (!$xdnoteProcess.WaitForExit(10000)) { $xdnoteProcess.Kill(); $xdnoteProcess.WaitForExit(); throw 'The fixture process exceeded its limit.' }
            $xdnoteExit=$xdnoteProcess.ExitCode
        } finally { $xdnoteProcess.Dispose() }
        if ($xdnoteExit -ne 0) { $xdnoteFirstFailure=$xdnoteAttempt; break }
    }
} catch {
    $xdnoteCollectionError = $_.Exception.Message
    throw
} finally {
    if ($xdnoteRecording) {
        & wpr.exe -stop (Join-Path $xdnoteRun 'rename.etl') 'xdnote fixture rename investigation' -instancename $xdnoteId -skipPdbGen *> (Join-Path $xdnoteRun 'stop.txt')
        if ($LASTEXITCODE -ne 0) {
            $xdnoteCollectionError = 'The dedicated WPR stop failed.'
            & wpr.exe -cancel -instancename $xdnoteId *> (Join-Path $xdnoteRun 'cancel.txt')
            Write-Error 'The dedicated WPR stop failed; cancellation was attempted for this instance only.' -ErrorAction Continue
        }
    }
    foreach ($xdnoteKey in $xdnoteKeys) { [Environment]::SetEnvironmentVariable($xdnoteKey,$xdnotePrevious[$xdnoteKey],'Process') }
    [ordered]@{ collectionError=$xdnoteCollectionError; firstFailure=$xdnoteFirstFailure; testExit=$xdnoteExit; executableSha256=$xdnoteHash; runDirectory=$xdnoteRun } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $xdnoteRun 'result.json') -Encoding utf8NoBOM
}
if ($xdnoteCollectionError) { throw $xdnoteCollectionError }
Write-Output "Kernel fixture evidence: $xdnoteRun"
