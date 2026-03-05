Build & Run Instructions for simulate_panning

MSVC (Visual Studio Build Tools / Developer Command Prompt):

1. Open "x64 Native Tools Command Prompt for VS 2022" (or Developer Command Prompt).
2. Change directory to the project folder:

   cd %USERPROFILE%\iGPU_Shim\synapse\tools

3. Build with cl or msbuild:

   cl /std:c++20 /O2 /I.. simulate_panning.cpp /Fe:simulate_panning.exe

   or

   msbuild simulate_panning.vcxproj /m /p:Configuration=Release

MinGW (if installed):

1. From a shell with MinGW on PATH:

   g++ -std=c++20 -O2 -I.. simulate_panning.cpp -o simulate_panning.exe

Notes:
- The simulation uses minimal Vulkan typedefs present in the repository headers; ensure includes resolve from project root (`/I..`).
- If you installed Visual Studio Build Tools, open the Developer Command Prompt or run `vcvarsall.bat` to set environment variables before building.
