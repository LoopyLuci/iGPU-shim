# Synapse Local Runner — NixOS Flake

Run Synapse builds/tests on a NixOS host, or prepare a local runner
environment without Docker. This flake is intended for native or VM
execution on a NixOS machine acting as a local network runner.

## Requirements

- Nix with flakes enabled
- Optional: `qemu`, `OVMF`, `win-spice` if running Windows tasks under QEMU/KVM

## Usage

```bash
# Enter a development shell with build/test tools
nix develop

# Native Linux build/test helpers available in shell hook:
#   build-native
#   test-native
```

## Notes

- This project builds on Windows via MSVC; on NixOS, prefer native Linux
  tasks or Windows VM execution.
- No external CI/CD is required. Local runner jobs should execute on
  personal hardware only.