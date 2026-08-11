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
echo [run_ctests] ctest --output-on-failure -C %BUILD_TYPE%
ctest --output-on-failure -C %BUILD_TYPE%
set CTEST_RC=%ERRORLEVEL%
popd

echo [run_ctests] Build type : %BUILD_TYPE%
echo [run_ctests] Build preset: %BUILD_PRESET%
echo [run_ctests] ctest exit code: %CTEST_RC%
if %CTEST_RC% EQU 0 (
    echo [run_ctests] PASS
) else (
    echo [run_ctests] FAIL
)
exit /b %CTEST_RC%
