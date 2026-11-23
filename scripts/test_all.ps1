# Runs the headless tests against each version's FRESH build dir (not the
# installed copy - NDK_NOTES 50), with -ti (interactive licence, NDK_NOTES 58).
#   powershell -ExecutionPolicy Bypass -File scripts\test_all.ps1 [-Perf]
param(
    [string[]]$Versions = @("14.1v8", "15.0v8", "15.1v10", "15.2v9", "16.0v8", "16.1v1", "17.0v4", "17.1v1"),
    [switch]$Perf
)

$root = Split-Path -Parent $PSScriptRoot
$results = @()

foreach ($v in $Versions) {
    $exe = Get-ChildItem "C:\Program Files\Nuke$v\Nuke*.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^Nuke[0-9.]+\.exe$' } | Select-Object -First 1
    $dll = Join-Path $root "build-$v\Release\MeshSimplify.dll"
    if (-not $exe) { $results += "SKIP  $v (no Nuke exe)"; continue }
    if (-not (Test-Path $dll)) { $results += "SKIP  $v (no build)"; continue }

    $env:MS_PLUGIN_DIR = (Join-Path $root "build-$v\Release").Replace('\', '/')
    $env:MS_TAG = $v
    Write-Host "=== Nuke $v ==="
    $tests = @("test_crop.py")
    if ($Perf) { $tests += "perf.py" }
    # Keep the script path short: Nuke 16+ crashes on long -ti paths (NDK_NOTES 53).
    foreach ($t in $tests) {
        & $exe.FullName -ti (Join-Path $root "test\$t") 2>&1 |
            Where-Object { $_ -match '^(FAIL|SUMMARY|PERF) ' -or $_ -match 'Traceback|Error' }
        if ($LASTEXITCODE -eq 0) { $results += "PASS  $v $t" }
        else { $results += "FAIL  $v $t (exit $LASTEXITCODE)" }
    }
}

$env:MS_PLUGIN_DIR = $null
$env:MS_TAG = $null

Write-Host ""
Write-Host "==== test_all summary ===="
$results | ForEach-Object { Write-Host $_ }
if ($results -match '^FAIL') { exit 1 }
