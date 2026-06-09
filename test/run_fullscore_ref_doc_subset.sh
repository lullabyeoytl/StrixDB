#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF_ROOT="${REF_ROOT:-/tmp/rmdb-fullscore-ref}"
SERVER="${PROJECT_ROOT}/build/bin/rmdb"

if [[ ! -x "${SERVER}" ]]; then
    echo "[FAIL] current server binary not found: ${SERVER}" >&2
    exit 1
fi

if [[ ! -f "${REF_ROOT}/tests/run_doc_tests.py" ]]; then
    echo "[FAIL] reference test runner not found: ${REF_ROOT}/tests/run_doc_tests.py" >&2
    echo "Set REF_ROOT to the full-score reference checkout." >&2
    exit 1
fi

python3 "${REF_ROOT}/tests/run_doc_tests.py" \
    --server "${SERVER}" \
    --build-dir "${PROJECT_ROOT}/build" \
    --show-server-log \
    04_01_selection_pushdown \
    04_02_projection_pushdown \
    04_03_join_order \
    04_04_optimizer_robust \
    04_05_explain_hidden_edges \
    04_06_equal_join_filter_propagation \
    04_07_transitive_equal_filter_propagation \
    04_09_two_table_no_reorder_diag \
    04_10_same_table_filter_join \
    04_11_self_join_dual_filters \
    04_12_multi_condition_second_join \
    04_13_two_table_right_filter_rhs \
    04_14_two_table_right_filter_reversed_on \
    04_15_two_table_comma_join_right_filter \
    04_16_two_table_both_filters_rhs_smaller \
    07_01_join_nlj_inlj \
    07_02_join_multitable \
    07_03_join_hidden_edges \
    07_04_join_partial_no_match \
    07_05_join_five_table \
    07_07_join_reversed_on \
    07_08_join_projection_order \
    07_09_join_ignore_wrong_right_index \
    07_10_join_left_index_only \
    07_11_join_multi_condition_residual \
    07_12_join_multi_condition_index_second \
    07_13_join_char_key_inlj \
    07_14_join_three_table_partial_prefix \
    07_15_join_three_table_first_key_second_join \
    07_16_join_three_table_first_no_match
