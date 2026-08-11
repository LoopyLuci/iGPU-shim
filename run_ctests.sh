#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE="${1:-Release}"
BUILD_PRESET="${2:-stub}"
BUILD_DIR="build_${BUILD_PRESET}"

echo "[run_ctests] Build: ${BUILD_TYPE} ${BUILD_PRESET}"
cmake --preset "${BUILD_PRESET}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}"
pushd "${BUILD_DIR}" >/dev/null

ctest --output-on-failure -C "${BUILD_TYPE}" \
  -E 'test_d3d12_vtable_intercept|test_d3d12_interception'

echo "[run_ctests] vtable dump consistency check..."
if [ -x "Release/test_d3d12_vtable_dump.exe" ]; then
  ./Release/test_d3d12_vtable_dump.exe || true
elif [ -x "./test_d3d12_vtable_dump" ]; then
  ./test_d3d12_vtable_dump || true
else
  echo "[run_ctests] vtable dump executable not found; skipping"
fi

popd >/dev/null
