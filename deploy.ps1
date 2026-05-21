<#
.SYNOPSIS
    Deploy downloaded MeshSimplify files into the project tree and build.

.DESCRIPTION
    Looks for the three project files in -Source (default C:\files), moves
    each into its correct location under -Project, then runs cmake
    configure + build for Nuke 14 (VS 2019, x64, Release).

    File mapping:
        CMakeLists.txt       -> <project>/CMakeLists.txt
        README.md            -> <project>/README.md
        NukeMeshSimplify.cpp -> <project>/src/NukeMeshSimplify.cpp

    Already-deployed files (present in destination, absent from source)
    are skipped silently, so re-running the script is safe.

.PARAMETER Source
    Folder containing the downloaded loose files.
    Default: <project>/files (i.e., a "files" subfolder of the project).

.PARAMETER Project
    Project root. Default: the script's own directory.

.PARAMETER NukePath
    Nuke install path. Default: auto-detect the newest Nuke14.* under
    C:\Program Files.

.PARAMETER Generator
    CMake generator. Default: "Visual Studio 16 2019" (Nuke 14 requires
    VS 2019, not 2022).

.PARAMETER Install
    Copy MeshSimplify.dll, menu.py, and ABOUT.txt to
    %USERPROFILE%\.nuke\MeshSimplify\ on success, and append a
    pluginAddPath registration to %USERPROFILE%\.nuke\menu.py.

.PARAMETER Package
    After a successful build, produce a distribution zip in <project>/dist/
    containing a MeshSimplify/ folder with the DLL, menu.py, and ABOUT.txt.
    Recipients drop the folder into their .nuke directory.

.PARAMETER Version
    Optional version string (e.g. "1.0"). Used in the package filename:
    MeshSimplify-v<version>.zip. If omitted, the date is used:
    MeshSimplify-YYYY-MM-DD.zip.

.PARAMETER Clean
    Wipe the build directory before configuring.

.PARAMETER SkipBuild
    Move files only; do not run cmake.

.EXAMPLE
    .\deploy.ps1

.EXAMPLE
    .\deploy.ps1 -Source D:\Downloads -Install -Clean

.EXAMPLE
    .\deploy.ps1 -Package -Version "1.0"

.EXAMPLE
    .\deploy.ps1 -SkipBuild        # just shuffle files into place
#>

[CmdletBinding()]
param(
    [string]$Source    = "",
    [string]$Project   = $PSScriptRoot,
    [string]$NukePath  = "",
    [string]$Generator = "Visual Studio 16 2019",
    [switch]$Install,
    [switch]$Package,
    [string]$Version   = "",
    [switch]$Clean,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}
function Write-Done([string]$msg) {
    Write-Host "    $msg" -ForegroundColor Green
}
function Write-Skip([string]$msg) {
    Write-Host "    $msg" -ForegroundColor DarkGray
}
function Write-Warn([string]$msg) {
    Write-Host "    $msg" -ForegroundColor Yellow
}

# Resolve to absolute path with forward slashes. CMake variables and
# Nuke knobs both prefer forward slashes -- TCL eats backslashes in
# string knobs ($variable, [cmd], \escape), so we use generic-style
# paths everywhere we pass them on.
function ConvertTo-ForwardPath([string]$p) {
    $abs = [System.IO.Path]::GetFullPath($p)
    return $abs.Replace('\', '/')
}

function Find-NukeInstall {
    $pf = "C:\Program Files"
    if (-not (Test-Path $pf)) { return $null }
    $hit = Get-ChildItem -Path $pf -Directory -Filter "Nuke14.*" `
        -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

# Append a pluginAddPath registration block to the user's main .nuke/menu.py,
# guarded by a marker comment so re-running is idempotent. Creates the file
# if it doesn't exist. Returns one of: 'created', 'appended', 'already-present'.
function Add-PluginPathToMenuPy {
    param(
        [Parameter(Mandatory=$true)][string]$MenuPyPath,
        [string]$SubfolderName = "MeshSimplify"
    )

    $marker = "# --- MeshSimplify plugin (auto-added by deploy.ps1) ---"
    $endMarker = "# --- end MeshSimplify plugin ---"

    # The block we append. Leading blank line ensures separation from any
    # prior content; trailing newline keeps the file tidy if we ever append
    # something else later.
    $block = @"

$marker
import nuke
nuke.pluginAddPath('./$SubfolderName')
$endMarker

"@

    if (Test-Path $MenuPyPath) {
        $existing = Get-Content $MenuPyPath -Raw -ErrorAction SilentlyContinue
        if ($existing -and $existing.Contains($marker)) {
            return 'already-present'
        }
        # Append; Add-Content handles encoding inheritance reasonably for
        # ASCII content like ours.
        Add-Content -Path $MenuPyPath -Value $block -Encoding ASCII
        return 'appended'
    } else {
        $header = @"
# .nuke/menu.py
# Nuke startup menu customizations. Created by MeshSimplify install.

"@
        Set-Content -Path $MenuPyPath -Value ($header + $block) -Encoding ASCII
        return 'created'
    }
}

# Build a distribution zip in <project>/dist/. The zip contains a
# MeshSimplify/ folder with the .dll, menu.py, and ABOUT.txt. Recipients
# extract the folder into their .nuke directory and add a pluginAddPath
# line to their menu.py (or run our deploy.ps1 -Install).
#
# Filename: MeshSimplify-v<version>.zip if -Version given, else
# MeshSimplify-YYYY-MM-DD.zip.
function New-DistributionPackage {
    param(
        [Parameter(Mandatory=$true)][string]$ProjectRoot,
        [Parameter(Mandatory=$true)][string]$DllPath,
        [string]$VersionStr = ""
    )

    $distRoot = Join-Path $ProjectRoot "dist"
    if (-not (Test-Path $distRoot)) {
        New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
    }

    # Staging: dist/.staging/MeshSimplify/ -- zipping this folder gives
    # us a zip whose root is MeshSimplify/.
    $stagingRoot   = Join-Path $distRoot ".staging"
    $stagingPlugin = Join-Path $stagingRoot "MeshSimplify"
    if (Test-Path $stagingRoot) {
        Remove-Item -Recurse -Force $stagingRoot
    }
    New-Item -ItemType Directory -Force -Path $stagingPlugin | Out-Null

    Copy-Item $DllPath (Join-Path $stagingPlugin "MeshSimplify.dll") -Force

    $menuSrc = Join-Path $ProjectRoot "menu.py"
    if (Test-Path $menuSrc) {
        Copy-Item $menuSrc (Join-Path $stagingPlugin "menu.py") -Force
    } else {
        Write-Warn "menu.py not found at $menuSrc -- zip will be missing it."
    }

    $aboutSrc = Join-Path $ProjectRoot "ABOUT.txt"
    if (Test-Path $aboutSrc) {
        Copy-Item $aboutSrc (Join-Path $stagingPlugin "ABOUT.txt") -Force
    } else {
        Write-Warn "ABOUT.txt not found at $aboutSrc -- zip will be missing it."
    }

    if ($VersionStr) {
        $zipName = "MeshSimplify-v$VersionStr.zip"
    } else {
        $stamp = Get-Date -Format "yyyy-MM-dd"
        $zipName = "MeshSimplify-$stamp.zip"
    }
    $zipPath = Join-Path $distRoot $zipName

    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path $stagingPlugin -DestinationPath $zipPath

    Remove-Item -Recurse -Force $stagingRoot
    return $zipPath
}

# -----------------------------------------------------------------------
# Validate inputs and resolve Nuke install
# -----------------------------------------------------------------------

Write-Step "Validating inputs"

if (-not $Project) {
    # $PSScriptRoot is empty when the script is dot-sourced or pasted
    # interactively. Fall back to the current location.
    $Project = (Get-Location).Path
}
if (-not (Test-Path $Project -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $Project | Out-Null
}
$Project = (Resolve-Path $Project).Path

# Source defaults to <project>/files. Resolve after Project so the user
# can pass -Source separately to override (absolute or relative path).
if (-not $Source) {
    $Source = Join-Path $Project "files"
}
if (-not (Test-Path $Source -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $Source | Out-Null
    Write-Done "Created source folder: $Source"
}

if (-not $NukePath) {
    $detected = Find-NukeInstall
    if ($detected) {
        $NukePath = $detected
    } else {
        $NukePath = "C:\Program Files\Nuke14.0v5"   # last-resort default
    }
}
if (-not (Test-Path $NukePath -PathType Container)) {
    Write-Warn "Nuke install not found at: $NukePath"
    Write-Warn "Build will fail. Re-run with -NukePath '<path>'."
}

Write-Done "Source:    $Source"
Write-Done "Project:   $Project"
Write-Done "Nuke:      $NukePath"
Write-Done "Generator: $Generator"

# -----------------------------------------------------------------------
# Move files into place
# -----------------------------------------------------------------------

$mapping = @(
    @{ Src = "CMakeLists.txt";       Dst = "CMakeLists.txt" }
    @{ Src = "README.md";            Dst = "README.md" }
    @{ Src = "NukeMeshSimplify.cpp"; Dst = "src/NukeMeshSimplify.cpp" }
    @{ Src = "menu.py";              Dst = "menu.py" }
    @{ Src = "ABOUT.txt";            Dst = "ABOUT.txt" }
)

Write-Step "Deploying files from $Source"

$moved           = 0
$alreadyDeployed = 0
$missing         = @()

foreach ($entry in $mapping) {
    $srcPath = Join-Path $Source  $entry.Src
    $dstPath = Join-Path $Project $entry.Dst

    if (-not (Test-Path $srcPath)) {
        if (Test-Path $dstPath) {
            Write-Skip "$($entry.Dst) (already in place)"
            $alreadyDeployed++
            continue
        }
        $missing += $entry.Src
        continue
    }

    $parent = Split-Path $dstPath -Parent
    if (-not (Test-Path $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    if (Test-Path $dstPath) { Remove-Item $dstPath -Force }
    Move-Item -Path $srcPath -Destination $dstPath
    Write-Done "$($entry.Src) -> $($entry.Dst)"
    $moved++
}

if ($missing.Count -gt 0) {
    Write-Host ""
    if ($alreadyDeployed -eq 0 -and $moved -eq 0) {
        # Nothing was found anywhere -- first-run case. Friendly exit.
        Write-Warn "No project files found yet."
        Write-Warn "Drop these into: $Source"
        foreach ($m in $missing) { Write-Warn "    $m" }
        Write-Warn ""
        Write-Warn "Then re-run: .\deploy.ps1"
        return
    } else {
        # Partial deploy -- some files were placed, others are missing.
        # That's a real problem.
        throw "Partial deploy: missing source files in ${Source}: $($missing -join ', ')"
    }
}
if ($moved -eq 0) {
    Write-Skip "All files already deployed; nothing to move."
}

# -----------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------

if ($SkipBuild) {
    Write-Step "Skipping build (-SkipBuild)"
    return
}

$buildDir = Join-Path $Project "build"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Step "Cleaning build directory"
    Remove-Item -Recurse -Force $buildDir
    Write-Done "Removed $buildDir"
}

Write-Step "Configuring (cmake)"

$cmakeArgs = @(
    "-G", $Generator
    "-A", "x64"
    "-DCMAKE_PREFIX_PATH=$(ConvertTo-ForwardPath $NukePath)"
    "-S", "$(ConvertTo-ForwardPath $Project)"
    "-B", "$(ConvertTo-ForwardPath $buildDir)"
)

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    throw "cmake configure failed (exit $LASTEXITCODE)"
}

Write-Step "Building (Release)"

& cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) {
    throw "cmake build failed (exit $LASTEXITCODE)"
}

# -----------------------------------------------------------------------
# Locate the produced DLL
# -----------------------------------------------------------------------

$dllCandidates = @(
    (Join-Path $buildDir "Release\MeshSimplify.dll")
    (Join-Path $buildDir "MeshSimplify.dll")
)
$dll = $dllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $dll) {
    throw "Build appeared to succeed but MeshSimplify.dll was not found under $buildDir"
}

Write-Step "Build complete"
Write-Done $dll

# -----------------------------------------------------------------------
# Optional install
# -----------------------------------------------------------------------

if ($Install) {
    $nukeUserDir = Join-Path $env:USERPROFILE ".nuke"
    if (-not (Test-Path $nukeUserDir)) {
        New-Item -ItemType Directory -Force -Path $nukeUserDir | Out-Null
    }

    # Distribution folder pattern: each plugin gets its own subfolder under
    # .nuke so its menu.py doesn't conflict with the user's main menu.py.
    $pluginDir = Join-Path $nukeUserDir "MeshSimplify"
    if (-not (Test-Path $pluginDir)) {
        New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
    }

    $dllTarget   = Join-Path $pluginDir "MeshSimplify.dll"
    $menuSrc     = Join-Path $Project "menu.py"
    $menuTarget  = Join-Path $pluginDir "menu.py"
    $aboutSrc    = Join-Path $Project "ABOUT.txt"
    $aboutTarget = Join-Path $pluginDir "ABOUT.txt"
    $mainMenuPy  = Join-Path $nukeUserDir "menu.py"

    # A running Nuke holds a file lock on plugin DLLs it has loaded. If the
    # copy fails with "in use", that's almost certainly why.
    try {
        Copy-Item $dll $dllTarget -Force
        Write-Step "Installed"
        Write-Done $dllTarget

        # Plugin's own menu.py goes alongside the .dll. Safe to overwrite
        # this one -- it's our file, not the user's.
        if (Test-Path $menuSrc) {
            Copy-Item $menuSrc $menuTarget -Force
            Write-Done $menuTarget
        } else {
            Write-Warn "menu.py not found at $menuSrc -- toolbar entry will not register."
        }

        # ABOUT.txt next to the plugin so end-users can find install
        # instructions without re-downloading.
        if (Test-Path $aboutSrc) {
            Copy-Item $aboutSrc $aboutTarget -Force
            Write-Done $aboutTarget
        }

        # Append pluginAddPath('./MeshSimplify') to the user's main
        # .nuke\menu.py. Idempotent: re-runs detect the marker comment and
        # do nothing.
        $status = Add-PluginPathToMenuPy -MenuPyPath $mainMenuPy
        switch ($status) {
            'created'         { Write-Done "$mainMenuPy (created)" }
            'appended'        { Write-Done "$mainMenuPy (appended)" }
            'already-present' { Write-Skip "$mainMenuPy (already registered)" }
        }

        Write-Done "In Nuke: 'Update All Plugins' or restart to pick up the change."
    }
    catch {
        Write-Host ""
        Write-Host "ERROR: Install failed: $_" -ForegroundColor Red
        Write-Warn "Most likely cause: Nuke is open and holding a lock on the DLL."
        Write-Warn "Close Nuke (or use 'Update All Plugins') and retry with -Install."
        throw
    }
}

# -----------------------------------------------------------------------
# Distribution package
# -----------------------------------------------------------------------

if ($Package) {
    Write-Step "Packaging"

    try {
        $zipPath = New-DistributionPackage `
            -ProjectRoot $Project `
            -DllPath     $dll `
            -VersionStr  $Version
        Write-Done $zipPath

        if ($Version) {
            Write-Done "Version-stamped as v$Version."
        } else {
            Write-Done "Tip: pass -Version `"1.0`" for a version-stamped filename."
        }
    }
    catch {
        Write-Host ""
        Write-Host "ERROR: Packaging failed: $_" -ForegroundColor Red
        throw
    }
}

Write-Host ""
