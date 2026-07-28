Installation & Automation

Files added:
- `scripts/install_deps.ps1` — Windows installer (winget/Chocolatey)
- `scripts/build_windows.ps1` — MSBuild helper (run from Developer Command Prompt)
- `scripts/install_deps.sh` — Linux (apt) installer for Ubuntu/Debian
- `.github/workflows/ci.yml` — CI build matrix (Windows + Ubuntu)

Quick start (Windows):
1. Open an elevated PowerShell.
2. Run: `.	ools\..\scripts\install_deps.ps1` to install prerequisites.
3. Open "x64 Native Tools Command Prompt for VS".
4. Run: `powershell -File ..\scripts\build_windows.ps1 -Configuration Release`

Quick start (Linux):
1. Run: `sudo ./scripts/install_deps.sh`
2. Build: `g++ -std=c++20 -O2 -I. synapse/tools/simulate_panning.cpp -o synapse/tools/simulate_panning`

CI: The repository includes a GitHub Actions workflow that builds on `windows-latest` and `ubuntu-latest`.

If you want a different automation shape (e.g. choco-only), let me know which target and I will add that next.
