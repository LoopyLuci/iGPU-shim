@echo off
REM ============================================================================
REM build_msvc.bat — Build Project Synapse with MSVC on Windows
REM ============================================================================
REM Usage: build_msvc.bat [Release|Debug] [stub|real]
REM ============================================================================

setlocal enabledelayedexpansion

set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release

set DMA_MODE=%2
if "%DMA_MODE%"=="" set DMA_MODE=stub

set BUILD_DIR=build_%DMA_MODE%

REM ── Set up MSVC environment ────────────────────────────────────────────────
set MSVC_ROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set MSVC_BIN=%MSVC_ROOT%\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64
set MSVC_LIB=%MSVC_ROOT%\VC\Tools\MSVC\14.44.35207\lib\x64
set MSVC_INCLUDE=%MSVC_ROOT%\VC\Tools\MSVC\14.44.35207\include

set WIN_SDK=C:\Program Files (x86)\Windows Kits\10
set WIN_SDK_VER=10.0.26100.0
set WIN_SDK_BIN=%WIN_SDK%\bin\%WIN_SDK_VER%\x64
set WIN_SDK_LIB_UM=%WIN_SDK%\Lib\%WIN_SDK_VER%\um\x64
set WIN_SDK_LIB_UCRT=%WIN_SDK%\Lib\%WIN_SDK_VER%\ucrt\x64
set WIN_SDK_INC=%WIN_SDK%\Include\%WIN_SDK_VER%

set PATH=%MSVC_BIN%;%WIN_SDK_BIN%;%PATH%
set LIB=%MSVC_LIB%;%WIN_SDK_LIB_UM%;%WIN_SDK_LIB_UCRT%
set INCLUDE=%MSVC_INCLUDE%;%WIN_SDK_INC%\ucrt;%WIN_SDK_INC%\shared;%WIN_SDK_INC%\winrt;%WIN_SDK_INC%\um

REM ── Vulkan SDK ─────────────────────────────────────────────────────────────
set VULKAN_SDK=C:\Users\limpi\iGPU_Shim\vulkan_sdk
REM ── Display environment ────────────────────────────────────────────────────
echo ╔════════════════════════════════════════════════════════════════════╗
echo ║  Building Project Synapse — %BUILD_TYPE% (MSVC 19.x)              ║
echo ║  DMA Mode: %DMA_MODE%                                                  ║
echo ╚════════════════════════════════════════════════════════════════════╝
echo.

REM ── Configure ──────────────────────────────────────────────────────────────
echo [build] Configuring CMake...
cmake -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_CXX_STANDARD=20 ^
    -DCMAKE_CXX_FLAGS="/w44010 /w44324 /w44100 /w44189 /w44530 /w44996 /EHsc" ^
    -DSYNAPSE_STUB_DMA=ON ^
    -DSYNAPSE_POWER_VERIFY=OFF

if errorlevel 1 (
    echo [build] CMake configuration FAILED
    exit /b 1
)

REM ── Build ──────────────────────────────────────────────────────────────────
echo.
echo [build] Compiling...
cmake --build %BUILD_DIR% --config %BUILD_TYPE% --parallel

if errorlevel 1 (
    echo [build] Build FAILED
    exit /b 1
)

echo.
echo ╔════════════════════════════════════════════════════════════════════╗
echo ║  BUILD SUCCEEDED                                                  ║
echo ║  Output: %BUILD_DIR%\                                            ║
echo ╚════════════════════════════════════════════════════════════════════╝

endlocal
