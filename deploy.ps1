<#
.SYNOPSIS
    One-stop wrapper: place downloaded files, build every Nuke version,
    optionally install and package.

.DESCRIPTION
    1. Moves any loose project files found in -Source (default <project>/files)
       into their place in the tree (re-running is safe).
    2. Builds MeshSimplify for each Nuke version (scripts\build_all.ps1,
       one build-<ver>/ per version, VS2019 x64).
    3. -Install: cmake --install of every build into %USERPROFILE%\.nuke
       (MeshSimplify\<major.minor>\MeshSimplify.dll + init.py dispatcher, an
       idempotent pluginAddPath block in .nuke\init.py, and cleanup of the
       v1.0 layout: root-level MeshSimplify.dll and the old menu.py block).
    4. -Package: dist\MeshSimplify-<version>-win64[-linux64].zip with LICENSE
       (required by MIT for the meshoptimizer attribution), ABOUT.txt,
       README.md and the Linux .so files if linux-bin\ exists
       (scripts\package.ps1).

    Linux builds: scripts/build_linux_docker.sh on a Linux host with the Linux
    Nuke installs; see docs/LINUX_BUILD.md.

.EXAMPLE
    .\deploy.ps1 -Install
.EXAMPLE
    .\deploy.ps1 -Package -Version 1.1.0
.EXAMPLE
    .\deploy.ps1 -Versions 14.1v8,16.0v8 -Clean
#>

[CmdletBinding()]
param(
    [string]$Source   = "",
    [string]$Project  = $PSScriptRoot,
    [string[]]$Versions = @(),
    [switch]$Install,
    [switch]$Package,
    [string]$Version  = "",
    [switch]$Clean,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Done([string]$msg) { Write-Host "    $msg" -ForegroundColor Green }
function Write-Skip([string]$msg) { Write-Host "    $msg" -ForegroundColor DarkGray }

if (-not $Project) { $Project = (Get-Location).Path }
$Project = (Resolve-Path $Project).Path
if (-not $Source) { $Source = Join-Path $Project "files" }

# -----------------------------------------------------------------------
# Move loose downloaded files into place
# -----------------------------------------------------------------------
$mapping = @(
    @{ Src = "CMakeLists.txt";       Dst = "CMakeLists.txt" }
    @{ Src = "README.md";            Dst = "README.md" }
    @{ Src = "NukeMeshSimplify.cpp"; Dst = "src/NukeMeshSimplify.cpp" }
    @{ Src = "menu.py";              Dst = "python/menu.py" }
    @{ Src = "init.py";              Dst = "python/init.py" }
    @{ Src = "ABOUT.txt";            Dst = "ABOUT.txt" }
    @{ Src = "LICENSE";              Dst = "LICENSE" }
)
if (Test-Path $Source -PathType Container) {
    Write-Step "Deploying files from $Source"
    $moved = 0
    foreach ($entry in $mapping) {
        $srcPath = Join-Path $Source  $entry.Src
        $dstPath = Join-Path $Project $entry.Dst
        if (-not (Test-Path $srcPath)) { continue }
        $parent = Split-Path $dstPath -Parent
        if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        if (Test-Path $dstPath) { Remove-Item $dstPath -Force }
        Move-Item -Path $srcPath -Destination $dstPath
        Write-Done "$($entry.Src) -> $($entry.Dst)"
        $moved++
    }
    if ($moved -eq 0) { Write-Skip "Nothing to move." }
}

if ($SkipBuild) { Write-Step "Skipping build (-SkipBuild)"; return }

# -----------------------------------------------------------------------
# Build (and install) every version
# -----------------------------------------------------------------------
Write-Step "Building"
$buildArgs = @{}
if ($Versions.Count -gt 0) { $buildArgs.Versions = $Versions }
if ($Install) { $buildArgs.Install = $true }
if ($Clean)   { $buildArgs.Clean = $true }
& (Join-Path $Project "scripts\build_all.ps1") @buildArgs
if ($LASTEXITCODE -ne 0) {
    throw "build_all failed (exit $LASTEXITCODE). If an install step failed, Nuke may be holding a lock on the DLL: close it and retry."
}
if ($Install) { Write-Done "Installed into $env:USERPROFILE\.nuke\MeshSimplify. Restart Nuke to pick it up." }

# -----------------------------------------------------------------------
# Package
# -----------------------------------------------------------------------
if ($Package) {
    Write-Step "Packaging"
    $pkgArgs = @{}
    if ($Version) { $pkgArgs.Version = $Version }
    & (Join-Path $Project "scripts\package.ps1") @pkgArgs
}
