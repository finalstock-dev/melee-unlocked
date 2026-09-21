param(
    [string]$Iso,
    [string]$ConnectCode,
    [ValidateSet("d3d12", "d3d11")]
    [string]$Backend = "d3d12",
    [ValidateSet("Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $Root

if (-not $Iso) { $Iso = $env:MELEE_ISO }
if (-not $Iso -and (Test-Path (Join-Path $Root "melee.iso"))) {
    $Iso = Join-Path $Root "melee.iso"
}
if (-not $Iso -or -not (Test-Path $Iso -PathType Leaf)) {
    throw "Set MELEE_ISO or pass -Iso with your local clean NTSC 1.02 ISO. The ISO is read locally and is never copied into the repository."
}
$Iso = (Resolve-Path $Iso).Path

if (-not $SkipBuild) {
    python tools/extract_dol.py $Iso build/main.dol
    if ($LASTEXITCODE -ne 0) { throw "DOL extraction failed." }
    python port/recomp/recomp.py --dol build/main.dol --gct-base 0x8065CC80
    if ($LASTEXITCODE -ne 0) { throw "Guest recompilation failed." }
    cmake -S . -B build-review -G "Visual Studio 17 2022" -A x64 -DMELEE_BUILD_EXPERIMENTAL_PORT=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
    cmake --build build-review --config $Configuration --target native_practice_model_test melee_port --parallel
    if ($LASTEXITCODE -ne 0) { throw "Native build failed." }
    ctest --test-dir build-review -C $Configuration -R native_practice_model --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Native-practice lifecycle tests failed." }
}

$Exe = Join-Path $Root "build-review/port/$Configuration/melee_port.exe"
if (-not (Test-Path $Exe -PathType Leaf)) {
    throw "Missing $Exe. Run without -SkipBuild first."
}

$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$ReportDir = Join-Path $Root "reports/native-practice-$Stamp"
New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null
$GameLog = Join-Path $ReportDir "melee_port.log"
$Summary = Join-Path $ReportDir "summary.txt"

if ($ConnectCode) {
    Set-Clipboard -Value $ConnectCode
    Write-Host "Connect code copied. In Training: press Tab, focus Direct, and paste it."
}

$GameArgs = @(
    "--iso", $Iso,
    "--threaded-renderer",
    "--fps", "120",
    "--frame-mode", "authored",
    "--backend", $Backend,
    "--scale", "auto",
    "--volume", "70",
    "--log-file", $GameLog
)

Write-Host "Diagnostic flow:"
Write-Host "  1. Enter Training, then press Tab."
Write-Host "  2. Type or Ctrl+V the Direct code and start searching."
Write-Host "  3. Close with Tab and continue practicing. Reopen once and test Cancel."
Write-Host "  4. Search again. Verify Match found -> normal online flow."
Write-Host "  5. For failure coverage, end the peer before gameplay; release A, then press A once."
Write-Host "Close the game when the run is complete. Logs will be summarized automatically."

& $Exe @GameArgs
$ExitCode = $LASTEXITCODE

$Markers = @()
if (Test-Path $GameLog) {
    $Markers = Select-String -Path $GameLog -Pattern "native practice:" | ForEach-Object { $_.Line }
}

$Lines = @(
    "Native practice diagnostic",
    "Date: $(Get-Date -Format o)",
    "Executable: $Exe",
    "Backend: $Backend",
    "Presentation: 120 fps authored; gameplay simulation remains 60 Hz",
    "Process exit code: $ExitCode",
    "",
    "Observed coordinator markers:"
)
if ($Markers.Count -eq 0) { $Lines += "(none)" } else { $Lines += $Markers }
$Lines += @(
    "",
    "Marker checks (observed, not inferred):",
    "Search started: $([bool]($Markers -match 'Direct search started'))",
    "Cancel cleanup: $([bool]($Markers -match 'search cancelled and connection cleaned up'))",
    "Online handoff: $([bool]($Markers -match 'handing off to normal online flow'))",
    "Pre-match failure: $([bool]($Markers -match 'pre-match failure'))",
    "Practice return: $([bool]($Markers -match 'Training configuration fields restored'))",
    "",
    "Manually record: focus/paste, held-A rejection, visible transition timing, practice configuration, and opponent type (native or stock Slippi)."
)
$Lines | Set-Content -Path $Summary -Encoding UTF8
Write-Host "Diagnostic summary: $Summary"
exit $ExitCode
