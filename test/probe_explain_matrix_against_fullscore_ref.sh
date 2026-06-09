#!/usr/bin/env bash

set -euo pipefail

CURRENT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF_ROOT="${REF_ROOT:-/tmp/rmdb-fullscore-ref}"
CURRENT_SERVER="${CURRENT_ROOT}/build/bin/rmdb"
CURRENT_CLIENT="${CURRENT_ROOT}/build/bin/rmdb_client"
REF_SERVER="${REF_ROOT}/build/bin/rmdb"
SHARED_CLIENT="${CURRENT_CLIENT}"

TMP_BASE="/tmp/explain_matrix_$$"
CURRENT_DB="${TMP_BASE}_current_db"
REF_DB="${TMP_BASE}_ref_db"
CURRENT_LOG="${TMP_BASE}_current.server.log"
REF_LOG="${TMP_BASE}_ref.server.log"
CURRENT_SELECT="${TMP_BASE}_current.select.out"
REF_SELECT="${TMP_BASE}_ref.select.out"
CURRENT_EXPLAIN="${TMP_BASE}_current.explain.out"
REF_EXPLAIN="${TMP_BASE}_ref.explain.out"
CURRENT_ERR="${TMP_BASE}_current.client.err"
REF_ERR="${TMP_BASE}_ref.client.err"
SQL_FILE="${TMP_BASE}.sql"

failures=0
cases_run=0

cleanup_server() {
    local pid="${1:-}"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
}

cleanup_db() {
    local db_path="${1:-}"
    if [[ -n "${db_path}" ]]; then
        rm -rf "${db_path}"
    fi
}

wait_port_free() {
    local tries=80
    while (( tries > 0 )); do
        if ! printf 'help;\n' | "${SHARED_CLIENT}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        tries=$((tries - 1))
    done
    return 1
}

wait_client_ready() {
    local client="${1}"
    local tries=80
    while (( tries > 0 )); do
        if printf 'help;\n' | "${client}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        tries=$((tries - 1))
    done
    return 1
}

run_suite() {
    local server="${1}"
    local client="${2}"
    local db_name="${3}"
    local server_log="${4}"
    local select_out="${5}"
    local explain_out="${6}"
    local client_err="${7}"

    if ! wait_port_free; then
        echo "[FAIL] port 8765 did not become free" >&2
        return 1
    fi

    cleanup_db "${db_name}"
    "${server}" "${db_name}" > "${server_log}" 2>&1 &
    local server_pid=$!

    if ! wait_client_ready "${client}"; then
        echo "[FAIL] server did not accept connections: ${server}" >&2
        cat "${server_log}" >&2 || true
        cleanup_server "${server_pid}"
        return 1
    fi

    awk 'BEGIN{mode="setup"} /^select /{mode="select"} /^explain analyze /{mode="explain"} {print > ("'"${TMP_BASE}"'." mode ".sql")}' "${SQL_FILE}"

    "${client}" < "${TMP_BASE}.setup.sql" > /dev/null 2> "${client_err}"
    "${client}" < "${TMP_BASE}.select.sql" > "${select_out}" 2>> "${client_err}"
    "${client}" < "${TMP_BASE}.explain.sql" > "${explain_out}" 2>> "${client_err}"

    cleanup_server "${server_pid}"
    if ! wait_port_free; then
        echo "[FAIL] port 8765 did not become free after shutdown" >&2
        return 1
    fi
}

emit_two_table_case() {
    local join_kind="${1}"
    local join_dir="${2}"
    local filter_mode="${3}"
    local projection_kind="${4}"
    local order_kind="${5}"
    local limit_kind="${6}"

    local projection='a.id, b.id'
    if [[ "${projection_kind}" == "star" ]]; then
        projection='*'
    fi

    local join_cond='a.k = b.k'
    if [[ "${join_dir}" == "reversed" ]]; then
        join_cond='b.k = a.k'
    fi

    local where_parts=()
    if [[ "${join_kind}" == "comma" ]]; then
        where_parts+=("${join_cond}")
    fi
    if [[ "${filter_mode}" == "left" || "${filter_mode}" == "both" ]]; then
        where_parts+=("a.va > 2")
    fi
    if [[ "${filter_mode}" == "right" || "${filter_mode}" == "both" ]]; then
        where_parts+=("b.vb > 5")
    fi

    local from_clause="from a join b on ${join_cond}"
    if [[ "${join_kind}" == "comma" ]]; then
        from_clause='from a, b'
    fi

    local where_clause=''
    if (( ${#where_parts[@]} > 0 )); then
        local joined=''
        for part in "${where_parts[@]}"; do
            if [[ -n "${joined}" ]]; then
                joined+=' and '
            fi
            joined+="${part}"
        done
        where_clause=" where ${joined}"
    fi

    local suffix=''
    if [[ "${order_kind}" == "desc" ]]; then
        suffix+=' order by b.id desc'
    fi
    if [[ "${limit_kind}" == "limit2" ]]; then
        suffix+=' limit 2'
    fi

    cat > "${SQL_FILE}" <<SQL
create table a (id int, k int, va int);
create table b (id int, k int, vb int);
insert into a values (1, 10, 1);
insert into a values (2, 20, 4);
insert into a values (3, 20, 7);
insert into b values (11, 10, 6);
insert into b values (12, 20, 3);
insert into b values (13, 20, 9);
select ${projection} ${from_clause}${where_clause}${suffix};
explain analyze select ${projection} ${from_clause}${where_clause}${suffix};
SQL
}

emit_index_join_case() {
    local key_kind="${1}"
    local join_dir="${2}"
    local residual_mode="${3}"

    if [[ "${key_kind}" == "int" ]]; then
        local join_cond='c.k = o.k'
        if [[ "${join_dir}" == "reversed" ]]; then
            join_cond='o.k = c.k'
        fi
        local on_extra=''
        if [[ "${residual_mode}" == "join_residual" || "${residual_mode}" == "double_residual" ]]; then
            on_extra=' and o.amount > 600'
        fi
        if [[ "${residual_mode}" == "double_residual" ]]; then
            on_extra+=' and o.flag = 1'
        fi
        cat > "${SQL_FILE}" <<SQL
create table c (id int, k int, tier int);
create table o (oid int, k int, amount int, flag int);
create index o(k);
insert into c values (1, 1, 1);
insert into c values (2, 2, 1);
insert into c values (3, 3, 2);
insert into o values (101, 1, 500, 1);
insert into o values (102, 1, 1200, 0);
insert into o values (103, 2, 900, 1);
insert into o values (104, 2, 1500, 1);
insert into o values (105, 3, 700, 0);
select c.id, o.oid from c join o on ${join_cond}${on_extra} where c.k < 3;
explain analyze select c.id, o.oid from c join o on ${join_cond}${on_extra} where c.k < 3;
SQL
        return
    fi

    cat > "${SQL_FILE}" <<'SQL'
create table u (uid int, email char(20), lvl int);
create table p (pid int, email char(20), active int);
create index p(email);
insert into u values (1, 'a@x', 1);
insert into u values (2, 'b@x', 2);
insert into u values (3, 'c@x', 3);
insert into p values (101, 'a@x', 1);
insert into p values (102, 'b@x', 0);
insert into p values (103, 'c@x', 1);
select u.uid, p.pid from u join p on u.email = p.email where u.lvl > 1 and p.active = 1;
explain analyze select u.uid, p.pid from u join p on u.email = p.email where u.lvl > 1 and p.active = 1;
SQL
}

emit_three_table_case() {
    local join_dir="${1}"
    local residual_mode="${2}"

    local second_join='o.oid = s.oid'
    if [[ "${join_dir}" == "reversed" ]]; then
        second_join='s.oid = o.oid'
    fi

    local second_extra=''
    if [[ "${residual_mode}" == "delayed" ]]; then
        second_extra=' and s.delayed = 1'
    elif [[ "${residual_mode}" == "delayed_and_small" ]]; then
        second_extra=' and s.delayed = 1 and s.sid < 300'
    fi

    cat > "${SQL_FILE}" <<SQL
create table c (cid int, region int);
create table o (oid int, cid int, amount int);
create table s (sid int, oid int, delayed int);
create index s(oid);
insert into c values (1, 1);
insert into c values (2, 1);
insert into c values (3, 2);
insert into o values (101, 1, 500);
insert into o values (102, 2, 900);
insert into o values (103, 3, 700);
insert into s values (201, 101, 0);
insert into s values (202, 102, 1);
insert into s values (203, 103, 0);
select c.cid, o.oid, s.sid from c join o on c.cid = o.cid join s on ${second_join}${second_extra} where c.region = 1;
explain analyze select c.cid, o.oid, s.sid from c join o on c.cid = o.cid join s on ${second_join}${second_extra} where c.region = 1;
SQL
}

emit_self_join_case() {
    local join_dir="${1}"
    local filter_mode="${2}"

    local join_cond='l.grp = r.grp'
    if [[ "${join_dir}" == "reversed" ]]; then
        join_cond='r.grp = l.grp'
    fi

    local where_parts=()
    if [[ "${filter_mode}" == "left" || "${filter_mode}" == "both" ]]; then
        where_parts+=("l.score > 70")
    fi
    if [[ "${filter_mode}" == "right" || "${filter_mode}" == "both" ]]; then
        where_parts+=("r.score < 80")
    fi

    local where_clause=''
    if (( ${#where_parts[@]} > 0 )); then
        local joined=''
        for part in "${where_parts[@]}"; do
            if [[ -n "${joined}" ]]; then
                joined+=' and '
            fi
            joined+="${part}"
        done
        where_clause=" where ${joined}"
    fi

    cat > "${SQL_FILE}" <<SQL
create table t (id int, grp int, score int, name char(8));
insert into t values (1, 10, 80, 'a');
insert into t values (2, 10, 60, 'b');
insert into t values (3, 20, 90, 'c');
insert into t values (4, 20, 75, 'd');
select l.name, r.name from t l join t r on ${join_cond}${where_clause};
explain analyze select l.name, r.name from t l join t r on ${join_cond}${where_clause};
SQL
}

emit_composite_join_case() {
    local join_dir="${1}"
    local right_filter_mode="${2}"

    local first_cond='l.k1 = r.k1'
    local second_cond='l.k2 = r.k2'
    if [[ "${join_dir}" == "reversed" ]]; then
        first_cond='r.k2 = l.k2'
        second_cond='r.k1 = l.k1'
    fi

    local where_clause=' where l.lv > 5'
    if [[ "${right_filter_mode}" == "with_right_filter" ]]; then
        where_clause+=' and r.rv < 80'
    fi

    cat > "${SQL_FILE}" <<SQL
create table l (id int, k1 int, k2 int, lv int);
create table r (id int, k1 int, k2 int, rv int);
insert into l values (1, 10, 100, 5);
insert into l values (2, 10, 200, 6);
insert into l values (3, 20, 100, 7);
insert into r values (11, 10, 100, 50);
insert into r values (12, 10, 200, 60);
insert into r values (13, 20, 100, 70);
insert into r values (14, 20, 200, 80);
select l.id, r.id from l join r on ${first_cond} and ${second_cond}${where_clause};
explain analyze select l.id, r.id from l join r on ${first_cond} and ${second_cond}${where_clause};
SQL
}

emit_multi_index_case() {
    local residual_mode="${1}"

    local on_extra=''
    if [[ "${residual_mode}" == "residual" ]]; then
        on_extra=' and r.flag = 1'
    elif [[ "${residual_mode}" == "double_residual" ]]; then
        on_extra=' and r.flag = 1 and r.score > 60'
    fi

    cat > "${SQL_FILE}" <<SQL
create table l (id int, k1 int, k2 int, lv int);
create table r (id int, k1 int, k2 int, flag int, score int);
create index r(k1, k2);
insert into l values (1, 10, 100, 5);
insert into l values (2, 10, 200, 6);
insert into l values (3, 20, 100, 7);
insert into r values (11, 10, 100, 1, 50);
insert into r values (12, 10, 200, 0, 60);
insert into r values (13, 20, 100, 1, 70);
insert into r values (14, 20, 200, 1, 80);
select l.id, r.id from l join r on l.k1 = r.k1 and l.k2 = r.k2${on_extra} where l.lv > 5;
explain analyze select l.id, r.id from l join r on l.k1 = r.k1 and l.k2 = r.k2${on_extra} where l.lv > 5;
SQL
}

compare_case() {
    local case_name="${1}"

    if [[ -n "${CASE_FILTER:-}" && "${case_name}" != *"${CASE_FILTER}"* ]]; then
        return
    fi

    cases_run=$((cases_run + 1))
    echo "=== MATRIX CASE: ${case_name} SELECT DIFF ==="
    run_suite "${CURRENT_SERVER}" "${CURRENT_CLIENT}" "${CURRENT_DB}" "${CURRENT_LOG}" "${CURRENT_SELECT}" "${CURRENT_EXPLAIN}" "${CURRENT_ERR}" || {
        failures=$((failures + 1))
        return
    }
    run_suite "${REF_SERVER}" "${SHARED_CLIENT}" "${REF_DB}" "${REF_LOG}" "${REF_SELECT}" "${REF_EXPLAIN}" "${REF_ERR}" || {
        failures=$((failures + 1))
        return
    }

    local select_diff=''
    select_diff="$(diff -u "${REF_SELECT}" "${CURRENT_SELECT}" || true)"
    local explain_diff=''
    explain_diff="$(diff -u "${REF_EXPLAIN}" "${CURRENT_EXPLAIN}" || true)"

    if [[ -n "${select_diff}" || -n "${explain_diff}" ]]; then
        failures=$((failures + 1))
        echo "${select_diff}"
        echo
        echo "=== MATRIX CASE: ${case_name} EXPLAIN DIFF ==="
        echo "${explain_diff}"
        echo
        echo "=== MATRIX CASE: ${case_name} SQL ==="
        cat "${SQL_FILE}"
        echo
        return
    fi

    echo
    echo "=== MATRIX CASE: ${case_name} EXPLAIN DIFF ==="
    echo
}

trap 'cleanup_server "${current_pid:-}"; cleanup_server "${ref_pid:-}"; cleanup_db "${CURRENT_DB}"; cleanup_db "${REF_DB}"; rm -rf "${CURRENT_LOG}" "${REF_LOG}" "${CURRENT_SELECT}" "${REF_SELECT}" "${CURRENT_EXPLAIN}" "${REF_EXPLAIN}" "${CURRENT_ERR}" "${REF_ERR}" "${SQL_FILE}" "${TMP_BASE}.setup.sql" "${TMP_BASE}.select.sql" "${TMP_BASE}.explain.sql"' EXIT

two_table_specs=(
    "two_explicit_normal_left_cols|explicit|normal|left|cols|none|none"
    "two_explicit_normal_right_cols|explicit|normal|right|cols|none|none"
    "two_explicit_reversed_both_cols|explicit|reversed|both|cols|none|none"
    "two_explicit_normal_both_star|explicit|normal|both|star|none|none"
    "two_comma_normal_both_cols|comma|normal|both|cols|none|none"
    "two_comma_reversed_both_star|comma|reversed|both|star|none|none"
)

for spec in "${two_table_specs[@]}"; do
    IFS='|' read -r case_name join_kind join_dir filter_mode projection_kind order_kind limit_kind <<< "${spec}"
    emit_two_table_case "${join_kind}" "${join_dir}" "${filter_mode}" "${projection_kind}" "${order_kind}" "${limit_kind}"
    compare_case "${case_name}"
done

index_specs=(
    "idx_int_plain|int|normal|none"
    "idx_int_join_residual|int|normal|join_residual"
    "idx_int_double_residual|int|normal|double_residual"
    "idx_int_reversed_residual|int|reversed|join_residual"
    "idx_char_filter|char|normal|filter"
)

for spec in "${index_specs[@]}"; do
    IFS='|' read -r case_name key_kind join_dir residual_mode <<< "${spec}"
    emit_index_join_case "${key_kind}" "${join_dir}" "${residual_mode}"
    compare_case "${case_name}"
done

three_table_specs=(
    "three_idx_second_delayed|normal|delayed"
    "three_idx_second_reversed_delayed|reversed|delayed"
    "three_idx_second_double_residual|normal|delayed_and_small"
)

for spec in "${three_table_specs[@]}"; do
    IFS='|' read -r case_name join_dir residual_mode <<< "${spec}"
    emit_three_table_case "${join_dir}" "${residual_mode}"
    compare_case "${case_name}"
done

self_join_specs=(
    "self_join_normal_left|normal|left"
    "self_join_reversed_both|reversed|both"
)

for spec in "${self_join_specs[@]}"; do
    IFS='|' read -r case_name join_dir filter_mode <<< "${spec}"
    emit_self_join_case "${join_dir}" "${filter_mode}"
    compare_case "${case_name}"
done

composite_specs=(
    "composite_normal_right_filter|normal|with_right_filter"
    "composite_reversed_no_right_filter|reversed|no_right_filter"
)

for spec in "${composite_specs[@]}"; do
    IFS='|' read -r case_name join_dir right_filter_mode <<< "${spec}"
    emit_composite_join_case "${join_dir}" "${right_filter_mode}"
    compare_case "${case_name}"
done

multi_index_specs=(
    "multi_index_plain|none"
    "multi_index_residual|residual"
    "multi_index_double_residual|double_residual"
)

for spec in "${multi_index_specs[@]}"; do
    IFS='|' read -r case_name residual_mode <<< "${spec}"
    emit_multi_index_case "${residual_mode}"
    compare_case "${case_name}"
done

if (( failures > 0 )); then
    echo "[FAIL] explain matrix probe found ${failures} divergent case(s) against fullscore ref across ${cases_run} case(s)" >&2
    exit 1
fi

echo "[PASS] explain matrix probe matched fullscore ref across ${cases_run} case(s)"
