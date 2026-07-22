#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"${PROJECT_ROOT}/build-aiv-repeat-registration"}
ASCEND_ROOT=${ASCEND_ROOT:-${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}}
export AIV_TEST_LOCAL_IP=${AIV_TEST_LOCAL_IP:-127.0.0.1}
export AIV_TEST_REMOTE_IP=${AIV_TEST_REMOTE_IP:-127.0.0.1}
export AIV_TEST_PORT=${AIV_TEST_PORT:-19003}

if [[ -z "${ASU_AIV_PROVIDER_ROOT:-}" ]]; then
    echo "ASU_AIV_PROVIDER_ROOT must point to the libumc.a install prefix." >&2
    exit 2
fi

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DRUNTIME_ENVIRONMENT=ascend \
    -DASCEND_ROOT="${ASCEND_ROOT}" \
    -DBUILD_UCM_STORE=OFF \
    -DBUILD_UCM_ASU=ON \
    -DBUILD_UNIT_TESTS=ON \
    -DBUILD_UCM_ASU_PROVIDER_AIV=ON \
    -DASU_AIV_PROVIDER_ROOT="${ASU_AIV_PROVIDER_ROOT}"

cmake --build "${BUILD_DIR}" --target aiv_repeat_registration.test -j"${BUILD_JOBS:-8}"

TEST_BINARY="${BUILD_DIR}/ucm/transport/kv/asu/aiv_repeat_registration.test"
if [[ ! -x "${TEST_BINARY}" ]]; then
    echo "Test binary not found: ${TEST_BINARY}" >&2
    exit 2
fi

set +e
"${TEST_BINARY}" --gtest_color=yes
TEST_STATUS=$?
set -e

if [[ ${TEST_STATUS} -eq 132 ]]; then
    echo "Test exited with SIGILL (exit 132)." >&2
    echo "The last '[AIV repeat registration]' line identifies the failing call." >&2
fi

exit "${TEST_STATUS}"
