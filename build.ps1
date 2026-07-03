# One-command build for LiteQuery (Windows, PowerShell).
#
#   .\build.ps1              configure + build (Release) + run tests
#   .\build.ps1 -Debug       Debug build
#   .\build.ps1 -Asan        Debug build with AddressSanitizer + UBSan (if available)
#   .\build.ps1 -NoTest      skip running ctest
#
# If you use the MSYS2 GCC toolchain, put its bin on PATH first:
#   $env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

param(
    [switch]$Debug,
    [switch]$Asan,
    [switch]$NoTest
)

$ErrorActionPreference = "Stop"

$buildType = if ($Debug -or $Asan) { "Debug" } else { "Release" }
$asanFlag  = if ($Asan) { "ON" } else { "OFF" }

# Prefer Ninja when available (single-config, fast, consistent); otherwise let
# CMake pick its platform default generator.
$genArgs = @()
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $genArgs = @("-G", "Ninja")
}

cmake -S . -B build $genArgs "-DCMAKE_BUILD_TYPE=$buildType" "-DLITEQUERY_ASAN=$asanFlag"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $NoTest) {
    ctest --test-dir build --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host ""
Write-Host "Built: build/liblitequery.a"
Write-Host "Try:   .\build\lq_demo.exe"
