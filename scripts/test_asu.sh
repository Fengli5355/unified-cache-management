#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

"${SCRIPT_DIR}/test_asu_build_modules.sh"
"${SCRIPT_DIR}/test_asu_router.sh"
"${SCRIPT_DIR}/test_asu_client_impl.sh"
"${SCRIPT_DIR}/test_asu_view_server.sh"
"${SCRIPT_DIR}/test_asu_smoke.sh"
"${SCRIPT_DIR}/test_asu_store.sh"
