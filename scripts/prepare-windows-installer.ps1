# Pinned official compiler preparation, isolated from the product runtime.
param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build')) + [IO.Path]::DirectorySeparatorChar
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (!$outputRoot.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Use a dedicated directory under this project build/' }
$ancestor = [IO.DirectoryInfo]::new($outputRoot)
while ($ancestor) {
    if ($ancestor.Exists -and ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Tool path cannot pass through a link' }
    $ancestor = $ancestor.Parent
}
New-Item -ItemType Directory -Force $outputRoot | Out-Null
if ((Get-Item -LiteralPath $outputRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Tool directory cannot be a link' }
$config = (Get-Content (Join-Path $PSScriptRoot 'installer-toolchain.json') -Raw | ConvertFrom-Json).inno_setup
function Get-PinnedFile($Url, $Hash, $Path) {
    if (!(Test-Path -LiteralPath $Path)) { Invoke-WebRequest -Uri $Url -OutFile $Path }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash) { throw 'Installer tool/material hash mismatch' }
}
$setup = Join-Path $outputRoot 'compiler-setup.exe'
Get-PinnedFile $config.url $config.sha256 $setup
$compiler = Join-Path $outputRoot 'compiler'
$process = Start-Process $setup -WindowStyle Hidden -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER','/NOICONS',('/DIR="'+$compiler+'"')) -Wait -PassThru
if ($process.ExitCode -ne 0) { throw 'Pinned installer compiler setup failed' }
foreach ($material in $config.materials) {
    Get-PinnedFile $material.url $material.sha256 (Join-Path $outputRoot $material.file)
}
if (!(Test-Path -LiteralPath (Join-Path $compiler 'ISCC.exe'))) { throw 'Installer compiler not found' }
