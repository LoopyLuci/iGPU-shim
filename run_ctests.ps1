[CmdletBinding()]
param(
    [string]$BuildType = "Release",
    [string]$BuildPreset = "stub"
)

$ErrorActionPreference = 'Stop'

Write-Host "[run_ctests] Build: $BuildType $BuildPreset"
& .\build_msvc.bat $BuildType $BuildPreset
if ($LASTEXITCODE -ne 0) {
    throw "Build failed"
}

Push-Location build_stub

$ctestLog = Join-Path $env:TEMP "ctest_run_$BuildType.log"
Write-Host "[run_ctests] ctest --output-on-failure -C $BuildType"
ctest --output-on-failure -C $BuildType *>&1 | Tee-Object -FilePath $ctestLog | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[run_ctests] CTest FAILED"
    Pop-Location
    exit 1
}

if (Test-Path .\Release\test_d3d12_vtable_dump.exe) {
    Write-Host "[run_ctests] Running D3D12 vtable index consistency check..."
    & .\Release\test_d3d12_vtable_dump.exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[run_ctests] vtable dump consistency check: FAIL"
        Pop-Location
        exit 1
    }
    Write-Host "[run_ctests] vtable dump consistency check: PASS"
} else {
    Write-Host "[run_ctests] vtable dump executable not found; skipping consistency check"
}

Pop-Location
exit 0
