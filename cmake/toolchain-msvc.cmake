# CMake toolchain for building on Windows with MSVC + custom Vulkan paths
# Usage: cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-msvc.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# MSVC compiler
set(CMAKE_C_COMPILER "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe")
set(CMAKE_CXX_COMPILER "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe")

# Vulkan paths
set(VULKAN_SDK "C:/Users/limpi/iGPU_Shim/vulkan_sdk" CACHE PATH "Vulkan SDK path")
set(VULKAN_INCLUDE_DIR "${VULKAN_SDK}/include" CACHE PATH "Vulkan include dir")
set(VULKAN_LIBRARY "C:/Windows/System32/vulkan-1.dll" CACHE FILEPATH "Vulkan library")
