<#
Build script for Windows using MSVC (Developer Command Prompt).

Usage:
  Open "x64 Native Tools Command Prompt for VS 2022" (or run vcvarsall.bat),
  then:
    .\scripts\build_windows.ps1 -Configuration Release
#>

param(
    [string]$Configuration = "Release"
)

function Write-Info($m){ Write-Host "[INFO] $m" -ForegroundColor Cyan }
function Write-Err($m){ Write-Host "[ERR] $m" -ForegroundColor Red }

Write-Info "Building simulate_panning with MSBuild (Configuration=$Configuration)..."

# Attempt to find msbuild in PATH
$msb = Get-Command msbuild.exe -ErrorAction SilentlyContinue
if ($msb -eq $null) {
    Write-Err "msbuild.exe not found on PATH. Open 'x64 Native Tools Command Prompt' or run vcvarsall.bat first."
    exit 2
}

Push-Location (Join-Path $PSScriptRoot "..")
$proj = Join-Path "synapse\tools" "simulate_panning.vcxproj"
if (-not (Test-Path $proj)) { Write-Err "Project file $proj not found."; Pop-Location; exit 1 }

& msbuild.exe $proj /m /p:Configuration=$Configuration /p:PlatformToolset=v143
if ($LASTEXITCODE -ne 0) { Write-Err "MSBuild failed."; Pop-Location; exit $LASTEXITCODE }

Write-Info "Build succeeded. Output under synapse\tools\bin\$Configuration\"
Pop-Location
