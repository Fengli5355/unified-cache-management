#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build-asu-test}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-}"
DOWNLOAD_DEPENDENCE="${DOWNLOAD_DEPENDENCE:-ON}"
BUILD_JOBS="${BUILD_JOBS:-}"

if [[ -z "${BUILD_JOBS}" ]]; then
    BUILD_JOBS=$(nproc)
fi

CONFIGURE_ARGS=(
    -S "${PROJECT_ROOT}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DBUILD_UNIT_TESTS=ON
    -DBUILD_UCM_STORE=ON
    -DBUILD_UCM_ASU=ON
    -DBUILD_UCM_SPARSE=OFF
    -DBUILD_UCM_MINDIE=OFF
    -DDOWNLOAD_DEPENDENCE="${DOWNLOAD_DEPENDENCE}"
)

if [[ -n "${CMAKE_GENERATOR}" ]]; then
    CONFIGURE_ARGS+=(-G "${CMAKE_GENERATOR}")
fi

cmake "${CONFIGURE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target asu_transport asu_client asustore --parallel "${BUILD_JOBS}"
