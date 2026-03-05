#!/usr/bin/env bash
# Install minimal build deps for Linux (Debian/Ubuntu)
set -euo pipefail

echo "[info] Updating apt and installing build-essential, clang, cmake, python3, git"
sudo apt-get update
sudo apt-get install -y build-essential clang cmake python3 python3-venv python3-pip git

echo "[info] Installed dependencies. Use ./scripts/build_unix.sh to build."
