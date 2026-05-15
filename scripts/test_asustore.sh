#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build-asustore-test}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
TEST_FILTER="${TEST_FILTER:-RingHashTableTest.*:MaglevTest.*:DistributedHashTableTest.*:ViewMgrTest.*}"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DBUILD_UNIT_TESTS=ON \
    -DBUILD_UCM_STORE=ON \
    -DBUILD_UCM_SPARSE=OFF \
    -DBUILD_UCM_MINDIE=OFF \
    -DDOWNLOAD_DEPENDENCE=ON

cmake --build "${BUILD_DIR}" --target ucmstore.test -j"$(nproc)"

"${BUILD_DIR}/ucm/store/test/ucmstore.test" --gtest_filter="${TEST_FILTER}"
