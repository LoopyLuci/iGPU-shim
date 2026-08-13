# Phase 10 Remaining Gaps

## Objective
Close production-grade validation gaps that are not covered by existing tests or CI.

## Open Items
1. **Power logging**
   - Status: implemented in telemetry types; runtime logging path not validated on Intel UHD 630
   - Blocked: hardware availability for long-run telemetry capture

2. **ITS eviction hooks**
   - Status: interface defined; eviction callbacks not exercised against real workload
   - Blocked: reproducible eviction workload + Intel UGD hardware validation

3. **Descriptor-set notifications**
   - Status: hook point stubbed in core; notification pipeline not validated
   - Blocked: D3D12/Vulkan real-device interception validation

4. **Live `report.json` end-to-end validation**
   - Status: `/api/report` now serves C++ `report.json` if present
   - Next: verify full application lifetime write path on real driver stack

5. **Linux/Clang 17 validation**
   - Status: Nix flake/devShell added
   - Next: run on Linux host to validate `clang-17` + Ninja build

## Validation Commands
```bash
# Linux/Clang 17
cmake -B build-linux -G Ninja -DCMAKE_C_COMPILER=clang-17 -DCMAKE_CXX_COMPILER=clang++-17
cmake --build build-linux
ctest --test-dir build-linux
```
