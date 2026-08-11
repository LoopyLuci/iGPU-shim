# ADR: Defer in-process COM vtable patching on MSVC

## Status
Accepted

## Context
Project Synapse needs a Windows D3D12 interception strategy. We evaluated
in-process COM vtable patching on MSVC because it avoids an external helper
DLL. Real hardware validation on Intel UHD Graphics 630 / Windows SDK
10.0.26100.0 showed correct vtable indices for the targeted draw and dispatch
methods, but runtime hooking still crashes (`exit 139`) even with
`__stdcall` trampolines and corrected indices.

## Decision
Treat in-process COM vtable patching as unstable on this toolchain. Prefer
the helper-DLL path for Windows D3D12 interception. Preserve the vtable
scaffolding and `test_d3d12_vtable_dump` for future re-evaluation.

## Consequences
- `SynapseD3D12Helper.dll` is the canonical Windows interception path.
- `test_d3d12_vtable_intercept` covers creation/setup without enabling real
  hook validation.
- Any future re-attempt must validate on the same hardware/SDK or newer
  before changing this ADR.

## References
- `docs/ROADMAP_PRODUCTION_GRADE.md`
- `plan.md`
- `synapse/synapse_d3d12_layer.cpp`
- `synapse/tools/test_d3d12_vtable_dump.cpp`
