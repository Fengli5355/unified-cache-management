#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/../../../.." && pwd)
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build-kv-test}"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DBUILD_UCM_ASU=ON \
    -DBUILD_UCM_STORE=OFF \
    -DBUILD_UNIT_TESTS=OFF \
    -DRUNTIME_ENVIRONMENT=ascend
cmake --build "${BUILD_DIR}" --target kv-test
