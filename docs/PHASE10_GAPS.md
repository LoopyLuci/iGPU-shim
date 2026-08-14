# Phase 10 Remaining Gaps

## Objective
Close production-grade validation gaps that are not covered by existing tests or CI.

## Completed
- **Live `report.json` end-to-end validation**
  - `/api/report` now serves C++ `report.json`
  - Verified via `scripts/test_report_e2e.py`

## Open Items
1. **Power logging**
   - Status: implemented in telemetry types; runtime logging path not validated on Intel UHD 630
   - Next: long-run telemetry capture on target hardware

2. **ITS eviction hooks**
   - Status: interface defined; eviction callbacks not exercised against real workload
   - Next: reproducible eviction workload + Intel UHD hardware validation

3. **Descriptor-set notifications**
   - Status: hook point stubbed in core; notification pipeline not validated
   - Next: D3D12/Vulkan real-device interception validation

4. **Linux/Clang 17 validation**
   - Status: `flake.nix` added with `clang_17`, `cmake`, `ninja`, Rust toolchain
   - Next: run on Linux host to validate `clang-17` + Ninja build
   - Commands:
     ```bash
     cmake -B build-linux -G Ninja -DCMAKE_C_COMPILER=clang-17 -DCMAKE_CXX_COMPILER=clang++-17
     cmake --build build-linux
     ctest --test-dir build-linux
     ```

## Deferred
- **Dashboard Rust HTTP integration tests**
  - Implemented in a feature branch; merged in `v0.15.0`
  - Merged because test harness works on Windows host

## Validation Status
- MSVC build: passing
- CTest: passing
- Rust cargo test: passing
- Linux/Clang 17: pending Linux host availability
- Hardware-only items: pending Intel UHD 630 validation
