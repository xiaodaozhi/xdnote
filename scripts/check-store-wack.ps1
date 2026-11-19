# Run the Microsoft certification kit on the exact unsigned candidate.
$ErrorActionPreference = 'Stop'
$work = (Resolve-Path 'build/store-candidate').Path
$package = Join-Path $work 'candidate.msix'
$report = Join-Path $work 'wack-report.xml'
$record = Get-Content -Raw (Join-Path $work 'msix-report.json') | ConvertFrom-Json
$digest = (Get-FileHash -Algorithm SHA256 $package).Hash.ToLowerInvariant()
if ($record.sha256 -ne $digest -or $record.signed -or $record.releaseIdentity.revision -ne $env:GITHUB_SHA) {
    throw 'WACK input must be the original unsigned MSIX from this revision'
}

$result = [ordered]@{
    commit = $env:GITHUB_SHA
    packageSha256 = $digest
    os = [Environment]::OSVersion.Version.ToString()
    sessionId = (Get-Process -Id $PID).SessionId
    toolVersion = $null
    exitCode = $null
    reportGenerated = $false
    state = 'preparation'
    error = $null
}
try {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($currentUser)
    if ($result.sessionId -eq 0 -or !$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'WACK requires an elevated, active Windows session'
    }
    $tool = "${env:ProgramFiles(x86)}\Windows Kits\10\App Certification Kit\appcert.exe"
    if (!(Test-Path -LiteralPath $tool)) {
        $setup = Join-Path $work 'winsdksetup.exe'
        Invoke-WebRequest 'https://go.microsoft.com/fwlink/?linkid=2349110' -OutFile $setup
        if ((Get-FileHash -Algorithm SHA256 $setup).Hash -ne '6fa0fa27db77a909f5ecb35183cb26a969a6775936780936fe239e4f9c66b458' -or
            (Get-AuthenticodeSignature -LiteralPath $setup).Status -ne 'Valid') {
            throw 'Windows SDK installer identity differs'
        }
        $installation = Start-Process -FilePath $setup -ArgumentList '/q /norestart /ceip off /features OptionId.WindowsSoftwareLogoToolkit' -Wait -PassThru
        if ($installation.ExitCode -notin 0, 3010 -or !(Test-Path -LiteralPath $tool)) {
            throw "WACK component installation failed (exit $($installation.ExitCode))"
        }
    }
    $result.toolVersion = (Get-Item -LiteralPath $tool).VersionInfo.FileVersion
    $reset = Start-Process -FilePath $tool -ArgumentList 'reset' -Wait -PassThru -NoNewWindow
    if ($reset.ExitCode -ne 0) { throw "WACK reset failed (exit $($reset.ExitCode))" }
    $result.state = 'running'
    $testArguments = 'test -appxpackagepath "{0}" -reportoutputpath "{1}"' -f $package, $report
    $test = Start-Process -FilePath $tool -ArgumentList $testArguments -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $work 'wack-stdout.log') -RedirectStandardError (Join-Path $work 'wack-stderr.log')
    $result.exitCode = $test.ExitCode
    $result.reportGenerated = Test-Path -LiteralPath $report
    $result.state = 'completed'
    if ((Get-FileHash -Algorithm SHA256 $package).Hash -ne $digest) { throw 'WACK input bytes changed' }
    if ($test.ExitCode -ne 0 -or !$result.reportGenerated) {
        throw "WACK did not complete successfully (exit $($test.ExitCode), report $($result.reportGenerated))"
    }
} catch {
    $result.error = $_.Exception.Message
    throw
} finally {
    $result | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $work 'wack-run.json')
}
