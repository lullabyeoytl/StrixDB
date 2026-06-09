#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF_ROOT="${REF_ROOT:-/tmp/rmdb-fullscore-ref}"

if [[ ! -x "${PROJECT_ROOT}/build/bin/rmdb" ]]; then
    echo "[FAIL] current server binary not found: ${PROJECT_ROOT}/build/bin/rmdb" >&2
    exit 1
fi

if [[ ! -x "${REF_ROOT}/build/bin/rmdb" ]]; then
    echo "[FAIL] reference server binary not found: ${REF_ROOT}/build/bin/rmdb" >&2
    echo "Set REF_ROOT to the full-score reference checkout and build it first." >&2
    exit 1
fi

cd "${PROJECT_ROOT}"

bash test/check_explain_regression_alignment.sh
bash test/compare_explain_against_fullscore_ref.sh
bash test/probe_explain_matrix_against_fullscore_ref.sh

echo "[PASS] explain_draft full-scope regression matched fullscore ref"
