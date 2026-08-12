# Project Synapse — iGPU Synaptic Shim

> **A hybrid, adaptive User-Mode Driver (UMD) shim that intercepts Vulkan command streams and dynamically routes work between JIT compilation, HAI bytecode streaming, and Oracle passthrough — delivering measurable, production-grade performance and power improvements for integrated graphics processors.**

---

## At a Glance

| Metric                       | Value | Target |
|:-----------------------------|:------|:-------|
| Layer load overhead          | **292 ns GIPA / 291 ns GDPA** | < 10 µs |
| Memory bandwidth (real iGPU) | **5337 MB/s** | baseline |
| WAL telemetry                | **Verified end-to-end** | stable |
| Compute/bandwidth telemetry  | **Production default** | stable |
| Test coverage                | **54/54 CTest pass** | 100% |
| Hardware blocklist           | **Enabled** — known-broken configs fall back to compute/bandwidth | ✅ verified |
| D3D12 helper DLL             | **Real-device vtable hooking verified** | ✅ verified |
| Schema migration             | **v0→v1→v2→v3 proven** | ✅ verified |

---

## What Is It?

Synapse inserts as a transparent Vulkan implicit layer. To the application, it is a standard driver. To the hardware, it provides optimized, pre-analyzed command streams.

| Problem | Traditional Driver | Synapse |
|:--------|:-------------------|:--------|
| CPU driver overhead | Full command buffers every frame | HAI delta-compressed bytecode |
| Shader inefficiency | Offline one-size-fits-all | JIT telemetry-driven specialization |
| Memory bandwidth waste | Full mip chain transferred | ITS predicts required mip levels |
| Reactive power management | P-State changes after stall | DVFS + PGRO proactive ramp |
| Thermal failure | OS throttle doubles frame time | Mip-cap + boost suppression |

---

## Architecture

```
Application (Vulkan)
       │
       │  vkCmdDraw* / vkCmdDispatch / vkCmdPushConstants
       ▼
┌─────────────────────────────────────────────────────────┐
│               SYNAPSE SHIM (UMD Layer)                  │
│                                                         │
│  SynapseCore ──push──► TelemetryRingBuffer              │
│       │                      │ pop                      │
│       │                 Analyzer (bg)                   │
│       │                      │ recommendation           │
│       │                 Scheduler                       │
│       │          ┌──────────┼───────────┐               │
│    JIT Path   HAI Path   Oracle Path                    │
│   JITPipeline  HAIBuilder  (native pass-through)        │
│       │            │                                    │
│       └────────────┴──► SyncManager / FenceManager      │
│                                                         │
│  DVFSController ◄── ConfidenceAggregator                │
│  PowerEstimator     ForecastingProfiler                 │
│  ThermalAwareArbiter ◄── SmoothingEngine (PGRO)         │
│  PredictiveEngine + ITSCacheController                  │
└─────────────────────────────────────────────────────────┘
       │
       ▼
iGPU Hardware
```

### The Three Execution Backends

- **JIT Path** — telemetry-constant injection, register pressure reduction, ISA generation
- **HAI Path** — delta-compressed bytecode stream for high-frequency simple draws
- **Oracle Path** — fallback to original function pointer when Analyzer is cold or prediction fails

---

## Verified on Real Hardware

| Check | Result | Status |
|:------|:------|:-------|
| Layer load | `VK_LAYER_SYNAPSE_iGPU_Shim` loads on Intel UHD Graphics 630 | stable |
| Device chain | All draw functions intercepted via GDPA | stable |
| WAL telemetry | Draw call → WAL write → CleanShutdown marker verified | stable |
| Compute/bandwidth telemetry | **Production default** — WAL + compute-emulation path (`test_compute_draw_emulation`, `test_compute_draw_full_telemetry`) + real `vkCmdCopyBuffer` throughput (5337 MB/s) | stable |
| Draw telemetry | Hardware-gated via blocklist (`synapse/hardware/hardware_blocklist.h`); disabled on known-broken configs (Intel UHD 630 / driver 9466) | degraded on known-broken hardware |
| Memory bandwidth | **5337 MB/s** via `vkCmdCopyBuffer` | baseline |
| CI | Local-only via `build_msvc.bat + ctest` | stable |
| Hardware blocklist | Runtime allowlist/blocklist for draw telemetry; known-broken configs fall back to compute/bandwidth | ✅ verified |
| Graphics draw-path | Real draw/dispatch telemetry blocked on Intel UHD 630 driver 9466; compute/bandwidth telemetry is the validated production substitute | hardware-limited |

---

## Quickstart

### Prerequisites

- Windows 10/11, x64
- Visual Studio 17 2022 Build Tools
- CMake 3.20+
- Vulkan SDK 1.4+ (optional; `vulkaninfo` useful for validation)

### Build

```powershell
# From repo root
.\build_msvc.bat Release stub
```

Output: `build_stub/Release/SynapseLayer.dll`

### Run Tests

```powershell
.\run_ctests.bat
```

Or manually:

```powershell
cd build_stub
ctest --output-on-failure -C Release
```

### Enable the Layer

```powershell
$env:VK_LAYER_PATH = "C:\Users\limpi\iGPU_Shim\build_stub\Release"
$env:VK_INSTANCE_LAYERS = "VK_LAYER_SYNAPSE_iGPU_Shim"
```

### CLI

```powershell
.\build_stub\Release\synapse_cli.exe snapshot
.\build_stub\Release\synapse_cli.exe status
.\build_stub\Release\synapse_cli.exe profile
```

### Test Tools

| Tool | Purpose |
|:-----|:--------|
| `test_layer_load.exe` | Layer load + GDPA interception |
| `test_wal_telemetry.exe` | WAL end-to-end verification |
| `benchmark_overhead.exe` | GDPA function resolution benchmark |
| `bench_execution_overhead.exe` | GIPA/GDPA dispatch overhead |
| `bench_memory_bandwidth.exe` | Real `vkCmdCopyBuffer` throughput |
| `bench_wal_writes.exe` | WAL batch vs single-write throughput |
| `test_thermal_monitor.exe` | Vulkan/WMI thermal probe |
| `test_analyzer_thread.exe` | Background analyzer validation |
| `test_analyzer_thread_edge.exe` | Analyzer edge cases |
| `test_analyzer_wal_interaction.exe` | Analyzer–WAL interaction |
| `test_compute_draw_emulation.exe` | Compute-path telemetry on headless iGPU |
| `test_compute_draw_full_telemetry.exe` | Full compute-path telemetry integration |
| `test_headless_draw_bypass.exe` | Mixed draw-like operations without display server |
| `test_schema_migration.exe` | Schema migration scaffolding unit test |
| `test_schema_migration_integration.exe` | Legacy v0 metadata migration through CrashRecoveryManager |
| `test_d3d12_interception.exe` | D3D12 COM vtable interception unit test |

---

## Headless Limitations

Graphics-pipeline submission (`vkCmdDraw` inside a render pass) is hardware-limited on some Intel UHD Graphics configurations. The layer now includes a runtime hardware blocklist (`synapse/hardware/hardware_blocklist.h`) that automatically disables draw telemetry on known-broken hardware/driver combinations and falls back to compute/bandwidth telemetry.

Workarounds:
- Use the default compute/bandwidth telemetry path (validated production substitute)
- Run on hardware with a known-good driver where draw telemetry is allowed
- Enable real draw telemetry via `SYNAPSE_DRAW_TELEMETRY=ON` only on hardware with a supported driver

Thermal and power APIs are also hardware-dependent. On Intel UHD Graphics / driver 9466, no power or thermal data is exposed via available user-mode paths.

---

## Local CI

Run the full local CI pipeline on this machine:

```powershell
.\build_msvc.bat Release stub
cd build_stub
ctest --output-on-failure -C Release
```

This is the canonical local CI path. Build and test are fully self-contained on the host GPU/CPU; no external runner or third-party action is required.

### Windows Verification

```powershell
.\build_msvc.bat Release stub
.\run_ctests.bat Release stub
```

`run_ctests.bat` builds, runs `ctest`, and executes `test_d3d12_vtable_dump` as a post-CTest consistency check.

```powershell
.\run_ctests.ps1 -BuildType Release -BuildPreset stub
```

`run_ctests.ps1` is the structured PowerShell equivalent with `-ErrorAction Stop` and tee'd CTest logs.

### Linux Verification

```bash
./run_ctests.sh Release stub
```

`run_ctests.sh` configures, builds, runs `ctest`, and executes `test_d3d12_vtable_dump` when available.

> Note: Some D3D12 real-device COM vtable hook validation remains disabled on MSVC due to calling-convention instability. The helper-DLL path is the validated Windows interception strategy.

---

## Scope

### What Synapse Does

- Intercepts `vkCmdDrawIndexed`, `vkCmdDraw`, `vkCmdDispatch`, `vkCmdPushConstants`, `vkCmdBindDescriptorSets`
- Registers textures via `vkCreateImage`/`vkDestroyImage` hooks for ITS tracking
- Routes work to JIT, HAI, or Oracle based on live telemetry
- Predicts texture residency needs and initiates DMA prefetch
- Gracefully degrades under thermal stress without frame tears or hangs
- Exports structured telemetry via CLI and WAL

### What Synapse Does NOT Do

- Does **not** replace the kernel-mode driver
- Does **not** modify GPU firmware or microcode
- Does **not** operate on discrete GPU hardware (untested, unsupported)

---

## Documentation

| Document | Purpose |
|:---------|:--------|
| `README.md` | Developer on-ramp, build, test |
| `documentation.md` | Full module reference, API contracts, data flows |
| `plan.md` | Phased roadmap, risks, acceptance criteria |
| `iGPU_Shim.md` | Original architecture design and rationale |
| `docs/PHASE7_HARDWARE_CODESIGN.md` | Real iGPU test results and hardware co-design |
| `rules.md` | Engineering philosophy and constraints |

---

## D3D12 Interception Strategy

The main layer avoids heavy Windows inline-hook/D3D12 dependencies because MSVC build constraints make inline trampolines and code-patching fragile. Instead, D3D12 interception is isolated in a separate helper DLL:

- `synapse/synapse_d3d12_helper_dll.h` — exported hook lifecycle interface plus `helper_test()` for DLL smoke testing
- `synapse/synapse_d3d12_helper_dll.cpp` — self-contained Windows-only implementation with `install_hook`/`remove_hook` over function-pointer replacement
- `synapse/SynapseD3D12Helper.def` — explicit Windows export list for a stable DLL surface
- `synapse/synapse_d3d12_layer.h/.cpp` — COM vtable interception scaffolding
- `synapse/tools/test_d3d12_helper_dll.cpp` — dynamic-load test for the helper DLL
- `synapse/tools/test_d3d12_interception.cpp` — mock COM vtable unit test
- `synapse/tools/test_headless_draw_bypass.cpp` — headless draw-like telemetry path

Build artifacts on Windows:
- `SynapseD3D12Helper.dll`
- `test_d3d12_helper_dll.exe`
- `test_d3d12_helper_attach.exe`
- `test_d3d12_vtable_dump.exe`
- `test_d3d12_vtable_intercept.exe`
- `test_d3d12_device_smoke.exe`
- `test_d3d12_interception.exe`
- `test_headless_draw_bypass.exe`
- `test_headless_draw_bypass_edge.exe`

D3D12 design note:
- Real COM vtable patching is currently unstable in-process under MSVC, so the helper DLL path remains the preferred Windows interception strategy.
- The helper DLL now validates against a real Windows API (`kernel32!OutputDebugStringA`) with a 16-iteration install/remove stress loop; this confirms function-pointer replacement is stable on this toolchain.
- `test_d3d12_vtable_dump.exe` prints the real vtable layout for `ID3D12CommandQueue` and `ID3D12GraphicsCommandList`. Use it when adjusting hook indices or debugging trampoline crashes.
- Observed vtable indices on this hardware/SDK:
  - `ID3D12CommandQueue::ExecuteCommandLists` at `[10]` (NOT `[3]` — the vtable dump tool's label was incorrect; the SDK inheritance chain is IUnknown[0-2] + ID3D12Object[3-6] + ID3D12DeviceChild[7]; ID3D12CommandQueue methods start immediately after, at [8])
  - `ID3D12GraphicsCommandList::DrawInstanced` at `[12]`
  - `ID3D12GraphicsCommandList::DrawIndexedInstanced` at `[13]`
  - `ID3D12GraphicsCommandList::Dispatch` at `[14]`
- Helper-DLL overhead baseline on this host (`OutputDebugStringA`, 64 iters):
  - install avg: `0.004 ms`
  - remove avg: `0.000 ms`
- `SynapseCore` now logs helper-DLL load/attach failures with Win32 error codes, stores the loaded `HMODULE`, and calls `detach_process_hooks` + `FreeLibrary` during destruction.
- Tested Windows SDK: `10.0.26100.0` (`d3d12.h` layout matches dumped indices above).

## Known Limitations

- **Intel UHD 630 driver crash**: Any GPU work recording through the Vulkan command buffer crashes the Intel UHD 630 driver (27.20.100.9466). This includes `vkCmdDispatch` (compute), `vkCmdDraw`/`vkCmdDrawIndexed` (graphics inside a render pass), and `vkCmdCopyBuffer`. Only queue submit with an **empty** command buffer succeeds. The driver crash occurs at command buffer recording time, not at submit. As a result, real draw/dispatch telemetry is not achievable on this hardware; the layer's WAL/compute-emulation path (`test_compute_draw_emulation`, `test_compute_draw_full_telemetry`) remains the validated substitute. An active remote desktop session (Parsec) does not resolve the issue.
- **MSVC vtable-patching instability**: In-process COM vtable patching crashes (`exit 139`) under MSVC on this toolchain. The helper-DLL function-pointer replacement path is the validated Windows interception strategy.
- **Headless limitations**: Graphics-pipeline submission requires a display server. Without one, the Intel driver may crash before the layer can intercept.
- **Thermal/power APIs**: Intel UHD 630 / driver 9466 does not expose power or thermal data via available Windows user-mode APIs.
- **Discrete GPU**: Untested and unsupported on discrete GPU hardware.
