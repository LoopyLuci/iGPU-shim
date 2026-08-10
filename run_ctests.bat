@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul
echo ============================================================
echo Running local CI: build_msvc.bat Release stub ^> ctest
echo ============================================================
call build_msvc.bat Release stub
if errorlevel 1 (
  echo [CI] Build failed with %errorlevel%
  exit /b 1
)
echo [CI] Build succeeded. Running ctest...
pushd build_stub
ctest --output-on-failure -C Release
set CTEST_RC=%errorlevel%
popd
if !CTEST_RC! equ 0 (
  echo [CI] ALL TESTS PASSED
) else (
  echo [CI] TESTS FAILED: %CTEST_RC%
)
exit /b %CTEST_RC%
