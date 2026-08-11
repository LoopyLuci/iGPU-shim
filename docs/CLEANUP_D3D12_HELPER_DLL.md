# D3D12 helper-DLL cleanup notes

`install_hook` injects `SynapseD3D12Helper.dll` into a remote process. There is no safe
local-only cleanup script for the injected module because:
- `GetModuleHandle`/`EnumProcessModules` only sees modules in the calling process
- forced remote `FreeLibrary` without reference-count coordination can crash the target

If you need cleanup:
1. restart the target application
2. on Windows, use Process Explorer to verify `SynapseD3D12Helper.dll` is no longer loaded
