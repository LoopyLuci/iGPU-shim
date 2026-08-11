@echo off
setlocal
set BUILD_TYPE=%1
set BUILD_PRESET=%2

if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release
if "%BUILD_PRESET%"=="" set BUILD_PRESET=stub

echo [run_ctests] Build: %BUILD_TYPE% %BUILD_PRESET%
call build_msvc.bat %BUILD_TYPE% %BUILD_PRESET%
if errorlevel 1 (
    echo [run_ctests] BUILD FAILED
    exit /b 1
)

pushd build_stub
set CTEST_LOG=%TEMP%\ctest_run_%BUILD_TYPE%.log
echo [run_ctests] ctest --output-on-failure -C %BUILD_TYPE%
ctest --output-on-failure -C %BUILD_TYPE% > "%CTEST_LOG%" 2>&1
set CTEST_RC=%ERRORLEVEL%

for /f "tokens=2 delims= " %%a in ('ctest -N -C %BUILD_TYPE% ^| findstr /R "Total Tests:"') do set TOTAL=%%a
for /f "tokens=3 delims= " %%a in ('findstr /C:"tests passed" "%CTEST_LOG%"') do set PASSED=%%a
for /f "tokens=3 delims= " %%a in ('findstr /C:"tests failed" "%CTEST_LOG%"') do set FAILED=%%a

echo [run_ctests] Total : %TOTAL%
echo [run_ctests] Passed: %PASSED%
echo [run_ctests] Failed: %FAILED%

if %CTEST_RC% NEQ 0 (
    echo [run_ctests] FAIL
    echo [run_ctests] Failure summary:
    findstr /R /C:"^[0-9][0-9]*\/[0-9][0-9]* Test" "%CTEST_LOG%" | findstr /V "Passed" || echo [run_ctests] No failed test details found in log
    echo [run_ctests] Log: %CTEST_LOG%
) else (
    echo [run_ctests] PASS
)

echo [run_ctests] Running D3D12 vtable index consistency check...
if exist Release\test_d3d12_vtable_dump.exe (
    Release\test_d3d12_vtable_dump.exe
    if errorlevel 1 (
        echo [run_ctests] vtable dump consistency check: FAIL
    ) else (
        echo [run_ctests] vtable dump consistency check: PASS
    )
) else (
    echo [run_ctests] vtable dump executable not found; skipping consistency check
)

popd

exit /b %CTEST_RC%
