# Stages dist/MeshSimplify-<version>-<platforms>/ from all built Nuke versions
# and zips it. Run after scripts\build_all.ps1. Linux binaries are added when
# linux-bin/<major.minor>/MeshSimplify.so exists (output of
# scripts/build_linux_docker.sh; extract its linux-bin.tar.gz in the repo root):
#   tar -xzf linux-bin.tar.gz
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.1.0
param(
    [string]$Version = "1.1.0",
    [string]$LinuxDir = "linux-bin"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dll = "MeshSimplify.dll"

$builds = Get-ChildItem (Join-Path $root "build-*") -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "Release\$dll") }
if (-not $builds) {
    throw "No build-<ver>/Release/$dll found. Run scripts\build_all.ps1 first."
}

$linuxRoot = Join-Path $root $LinuxDir
$soDirs = Get-ChildItem $linuxRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^[0-9]+\.[0-9]+$' -and
                   (Test-Path (Join-Path $_.FullName "MeshSimplify.so")) }
$platforms = if ($soDirs) { "win64-linux64" } else { "win64" }

$name = "MeshSimplify-$Version-$platforms"
$top = Join-Path $root "dist\$name"
$stage = Join-Path $top "MeshSimplify"
if (Test-Path $top) { Remove-Item $top -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

$nukeVersions = @()
foreach ($b in $builds) {
    # build-16.0v8 -> 16.0
    if ($b.Name -match '^build-([0-9]+\.[0-9]+)v') {
        $mm = $Matches[1]
        $nukeVersions += $mm
        New-Item -ItemType Directory -Force (Join-Path $stage $mm) | Out-Null
        Copy-Item (Join-Path $b.FullName "Release\$dll") (Join-Path $stage $mm)
    }
}
$nukeVersions = $nukeVersions | Sort-Object -Unique

# Linux .so files go next to the .dll in the same <major.minor>/ folder; each
# Nuke only loads the binary with its own platform's extension.
$linuxVersions = @()
foreach ($d in $soDirs) {
    $dst = Join-Path $stage $d.Name
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item (Join-Path $d.FullName "MeshSimplify.so") $dst
    $linuxVersions += $d.Name
}
$linuxVersions = $linuxVersions | Sort-Object -Unique
$linuxLine = if ($linuxVersions) { $linuxVersions -join ', ' } else { "(none)" }

# LICENSE is required: MIT asks for meshoptimizer's notice to travel with the
# binary (NDK_NOTES 8.3).
foreach ($f in "python\init.py", "python\menu.py", "ABOUT.txt", "README.md", "docs\LINUX_BUILD.md", "LICENSE") {
    $src = Join-Path $root $f
    if (Test-Path $src) { Copy-Item $src $stage }
    elseif ($f -eq "LICENSE") { throw "LICENSE missing: required for the meshoptimizer attribution." }
}

@"
MeshSimplify $Version
Windows x64 Nuke versions: $($nukeVersions -join ', ')
Linux x64 Nuke versions:   $linuxLine

Crop classic-3D geometry to a box / Cube and to a camera frustum (with
overscan, optionally the union over the whole camera move), then simplify it
UV- and seam-aware with meshoptimizer, optionally merging flat triangle pairs
into quads.

INSTALL
1. Copy the MeshSimplify folder (the one containing init.py) into your .nuke
   folder:
     Windows:  %USERPROFILE%\.nuke\MeshSimplify
     Linux:    ~/.nuke/MeshSimplify
   If an older MeshSimplify.dll sits directly in that folder (v1.0 layout),
   delete it: it would be loaded instead of the per-version binary.
2. Add this line to init.py in that .nuke folder (create the file if needed):

     nuke.pluginAddPath('./MeshSimplify')

   (v1.0 put this line in menu.py; remove it from there.)
3. Restart Nuke. MeshSimplify appears under 3D > Modify. The right binary for
   your platform and Nuke version is picked automatically at startup.
   Linux needs glibc 2.28 or newer (Rocky / RHEL 8 or later).

Classic 3D system only (not GeoImport / USD nodes). See README.md.
"@ | Set-Content (Join-Path $top "INSTALL.txt")

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# Not Compress-Archive: Windows PowerShell 5.1 stores entries with '\', which
# Linux unzip extracts as flat files named "MeshSimplify\init.py".
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    $topFull = (Resolve-Path $top).Path.TrimEnd('\') + '\'
    Get-ChildItem $top -Recurse -File | ForEach-Object {
        $entry = $_.FullName.Substring($topFull.Length).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $_.FullName, $entry, 'Optimal') | Out-Null
    }
} finally {
    $archive.Dispose()
}

Write-Host "Staged: dist\$name"
Write-Host "  win64:   $($nukeVersions -join ', ')"
Write-Host "  linux64: $linuxLine"
Write-Host "Zipped: dist\$name.zip"
