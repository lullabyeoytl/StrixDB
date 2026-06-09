#!/usr/bin/env bash

set -euo pipefail

CURRENT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF_ROOT="${REF_ROOT:-/tmp/rmdb-fullscore-ref}"
CURRENT_SERVER="${CURRENT_ROOT}/build/bin/rmdb"
CURRENT_CLIENT="${CURRENT_ROOT}/build/bin/rmdb_client"
REF_SERVER="${REF_ROOT}/build/bin/rmdb"
# Both servers speak the same socket protocol, so one client binary is enough for comparison.
SHARED_CLIENT="${CURRENT_CLIENT}"

TMP_BASE="/tmp/explain_compare_$$"
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
    local client="${1}"
    local tries=80
    while (( tries > 0 )); do
        if ! printf 'help;\n' | "${client}" >/dev/null 2>&1; then
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

write_case_sql() {
    local case_name="${1}"
    case "${case_name}" in
        single_filter)
            cat > "${SQL_FILE}" <<'SQL'
create table t (a int, b int);
insert into t values (1, 5);
insert into t values (2, 8);
insert into t values (3, 12);
insert into t values (4, 6);
insert into t values (5, 20);
select a, b from t where a > 1 and b < 10;
explain analyze select a, b from t where a > 1 and b < 10;
SQL
            ;;
        hidden_edges)
            cat > "${SQL_FILE}" <<'SQL'
create table a (id int, k int, v int);
create table b (id int, k int, v int);
insert into a values (1, 10, 5);
insert into a values (2, 20, 9);
insert into a values (3, 30, 2);
insert into b values (101, 10, 7);
insert into b values (102, 20, 3);
insert into b values (103, 20, 8);
select a.id, b.id from a join b on a.k = b.k where b.v > 5 and a.v > 4;
explain analyze select a.id, b.id from a join b on a.k = b.k where b.v > 5 and a.v > 4;
explain analyze select a.id, a.k from a where a.v > 1 and a.id > 1;
SQL
            ;;
        customer_orders)
            cat > "${SQL_FILE}" <<'SQL'
create table orders (order_id int, customer_id int, order_date char(40), total_amount float);
create table customers (customer_id int, name char(50), email char(100), address char(200));
insert into customers values (1, 'Alice', 'alice@example.com', 'A Street');
insert into customers values (2, 'Bob', 'bob@example.com', 'B Street');
insert into customers values (3, 'Carol', 'carol@example.com', 'C Street');
insert into orders values (101, 1, '2025-01-01', 500.0);
insert into orders values (102, 1, '2025-01-02', 1200.0);
insert into orders values (103, 2, '2025-01-03', 900.0);
insert into orders values (104, 2, '2025-01-04', 1500.0);
insert into orders values (105, 3, '2025-01-05', 700.0);
select * from customers c join orders o on c.customer_id = o.customer_id where o.total_amount > 1000;
select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id;
explain analyze select * from customers c join orders o on c.customer_id = o.customer_id where o.total_amount > 1000;
explain analyze select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id;
SQL
            ;;
        three_join)
            cat > "${SQL_FILE}" <<'SQL'
create table orders (order_id int, customer_id int, order_date char(40), total_amount float);
create table customers (customer_id int, name char(50), email char(100), address char(200));
create table shipments (ship_id int, order_id int, status char(16));
insert into customers values (1, 'Alice', 'alice@example.com', 'A Street');
insert into customers values (2, 'Bob', 'bob@example.com', 'B Street');
insert into customers values (3, 'Carol', 'carol@example.com', 'C Street');
insert into orders values (101, 1, '2025-01-01', 500.0);
insert into orders values (102, 1, '2025-01-02', 1200.0);
insert into orders values (103, 2, '2025-01-03', 900.0);
insert into orders values (104, 2, '2025-01-04', 1500.0);
insert into orders values (105, 3, '2025-01-05', 700.0);
insert into shipments values (501, 101, 'packed');
insert into shipments values (502, 102, 'ready');
insert into shipments values (503, 103, 'sent');
insert into shipments values (504, 104, 'held');
select c.name, o.order_id, s.ship_id from customers c join orders o on c.customer_id = o.customer_id join shipments s on o.order_id = s.order_id where c.customer_id < 3 and s.ship_id > 501;
explain analyze select c.name, o.order_id, s.ship_id from customers c join orders o on c.customer_id = o.customer_id join shipments s on o.order_id = s.order_id where c.customer_id < 3 and s.ship_id > 501;
SQL
            ;;
        self_join_alias)
            cat > "${SQL_FILE}" <<'SQL'
create table t469 (id int, grp int, score int, name char(8));
insert into t469 values (1, 10, 80, 'a');
insert into t469 values (2, 10, 60, 'b');
insert into t469 values (3, 20, 90, 'c');
select l.name, r.name from t469 l join t469 r on l.grp = r.grp where l.score > 70 and r.score < 70;
explain analyze select l.name, r.name from t469 l join t469 r on l.grp = r.grp where l.score > 70 and r.score < 70;
SQL
            ;;
        reversed_join_direction)
            cat > "${SQL_FILE}" <<'SQL'
create table customers (customer_id int, name char(50), email char(100), address char(200));
create table orders (order_id int, customer_id int, order_date char(40), total_amount float);
insert into customers values (1, 'Alice', 'alice@example.com', 'A Street');
insert into customers values (2, 'Bob', 'bob@example.com', 'B Street');
insert into customers values (3, 'Carol', 'carol@example.com', 'C Street');
insert into orders values (101, 1, '2025-01-01', 500.0);
insert into orders values (102, 1, '2025-01-02', 1200.0);
insert into orders values (103, 2, '2025-01-03', 900.0);
insert into orders values (104, 2, '2025-01-04', 1500.0);
insert into orders values (105, 3, '2025-01-05', 700.0);
select c.name, o.order_id from customers c join orders o on o.customer_id = c.customer_id where c.customer_id = 2;
explain analyze select c.name, o.order_id from customers c join orders o on o.customer_id = c.customer_id where c.customer_id = 2;
SQL
            ;;
        comma_join_right_filter)
            cat > "${SQL_FILE}" <<'SQL'
create table a (id int, k int, va int);
create table b (id int, k int, vb int);
insert into a values (1, 10, 1);
insert into a values (2, 20, 4);
insert into a values (3, 20, 7);
insert into b values (11, 10, 6);
insert into b values (12, 20, 3);
insert into b values (13, 20, 9);
select a.id, b.id from a, b where b.k = a.k and a.va > 2 and b.vb > 5;
explain analyze select a.id, b.id from a, b where b.k = a.k and a.va > 2 and b.vb > 5;
SQL
            ;;
        wildcard_no_leaf_project)
            cat > "${SQL_FILE}" <<'SQL'
create table left_t (id int, k int, payload int);
create table right_t (rid int, k int, score int);
insert into left_t values (1, 10, 0);
insert into left_t values (2, 20, 2);
insert into left_t values (3, 20, 5);
insert into right_t values (101, 10, 8);
insert into right_t values (102, 20, 7);
insert into right_t values (103, 20, 10);
select * from left_t l join right_t r on l.k = r.k where l.payload > 1 and r.score < 9;
explain analyze select * from left_t l join right_t r on l.k = r.k where l.payload > 1 and r.score < 9;
SQL
            ;;
        propagated_eq_filter)
            cat > "${SQL_FILE}" <<'SQL'
create table customers (customer_id int, name char(50), email char(100), address char(200));
create table orders (order_id int, customer_id int, order_date char(40), total_amount float);
insert into customers values (1, 'Alice', 'alice@example.com', 'A Street');
insert into customers values (2, 'Bob', 'bob@example.com', 'B Street');
insert into customers values (3, 'Carol', 'carol@example.com', 'C Street');
insert into orders values (101, 1, '2025-01-01', 500.0);
insert into orders values (102, 1, '2025-01-02', 1200.0);
insert into orders values (103, 2, '2025-01-03', 900.0);
insert into orders values (104, 2, '2025-01-04', 1500.0);
insert into orders values (105, 3, '2025-01-05', 700.0);
select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id where c.customer_id = 2 and o.total_amount > 800;
explain analyze select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id where c.customer_id = 2 and o.total_amount > 800;
SQL
            ;;
        three_join_transitive_filter)
            cat > "${SQL_FILE}" <<'SQL'
create table ta (id int, k int, va int);
create table tb (id int, k int, vb int);
create table tc (id int, k int, vc int);
insert into ta values (1, 10, 5);
insert into ta values (2, 20, 6);
insert into ta values (3, 30, 7);
insert into tb values (11, 10, 1);
insert into tb values (12, 20, 2);
insert into tb values (13, 20, 3);
insert into tb values (14, 30, 4);
insert into tc values (21, 10, 1);
insert into tc values (22, 20, 5);
insert into tc values (23, 20, -1);
insert into tc values (24, 30, 8);
select a.id, b.id, c.id from ta a join tb b on a.k = b.k join tc c on b.k = c.k where a.k = 20 and c.vc > 0;
explain analyze select a.id, b.id, c.id from ta a join tb b on a.k = b.k join tc c on b.k = c.k where a.k = 20 and c.vc > 0;
SQL
            ;;
        projection_ordering_alias)
            cat > "${SQL_FILE}" <<'SQL'
create table alpha (id int, k int, m int);
create table beta (id int, k int, n int);
insert into alpha values (1, 10, 100);
insert into alpha values (2, 20, 200);
insert into beta values (11, 10, 900);
insert into beta values (12, 20, 800);
select b.n, a.id, b.id from alpha a join beta b on a.k = b.k;
explain analyze select b.n, a.id, b.id from alpha a join beta b on a.k = b.k;
SQL
            ;;
        indexed_right_join)
            cat > "${SQL_FILE}" <<'SQL'
create table customers (customer_id int, name char(50), tier int);
create table orders (order_id int, customer_id int, total_amount int);
create index orders(customer_id);
insert into customers values (1, 'Alice', 1);
insert into customers values (2, 'Bob', 1);
insert into customers values (3, 'Carol', 2);
insert into orders values (101, 1, 500);
insert into orders values (102, 1, 1200);
insert into orders values (103, 2, 900);
insert into orders values (104, 2, 1500);
insert into orders values (105, 3, 700);
select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id where c.customer_id < 3;
explain analyze select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id where c.customer_id < 3;
SQL
            ;;
        indexed_right_join_with_residual)
            cat > "${SQL_FILE}" <<'SQL'
create table customers (customer_id int, name char(50), tier int);
create table orders (order_id int, customer_id int, total_amount int);
create index orders(customer_id);
insert into customers values (1, 'Alice', 1);
insert into customers values (2, 'Bob', 1);
insert into customers values (3, 'Carol', 2);
insert into orders values (101, 1, 500);
insert into orders values (102, 1, 1200);
insert into orders values (103, 2, 900);
insert into orders values (104, 2, 1500);
insert into orders values (105, 3, 700);
select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id and o.total_amount > 1000 where c.customer_id < 3;
explain analyze select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id and o.total_amount > 1000 where c.customer_id < 3;
SQL
            ;;
        composite_join_conditions)
            cat > "${SQL_FILE}" <<'SQL'
create table left_pair (id int, k1 int, k2 int, lv int);
create table right_pair (id int, k1 int, k2 int, rv int);
insert into left_pair values (1, 10, 100, 5);
insert into left_pair values (2, 10, 200, 6);
insert into left_pair values (3, 20, 100, 7);
insert into right_pair values (11, 10, 100, 50);
insert into right_pair values (12, 10, 200, 60);
insert into right_pair values (13, 20, 100, 70);
insert into right_pair values (14, 20, 200, 80);
select l.id, r.id from left_pair l join right_pair r on r.k2 = l.k2 and l.k1 = r.k1 where l.lv > 5 and r.rv < 80;
explain analyze select l.id, r.id from left_pair l join right_pair r on r.k2 = l.k2 and l.k1 = r.k1 where l.lv > 5 and r.rv < 80;
SQL
            ;;
        indexed_right_join_two_residuals)
            cat > "${SQL_FILE}" <<'SQL'
create table customers (customer_id int, name char(50), tier int);
create table orders (order_id int, customer_id int, total_amount int, status char(10));
create index orders(customer_id);
insert into customers values (1, 'Alice', 1);
insert into customers values (2, 'Bob', 1);
insert into customers values (3, 'Carol', 2);
insert into orders values (101, 1, 500, 'ready');
insert into orders values (102, 1, 1200, 'hold');
insert into orders values (103, 2, 900, 'ready');
insert into orders values (104, 2, 1500, 'ready');
insert into orders values (105, 3, 700, 'hold');
select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id and o.total_amount > 600 and o.status = 'ready' where c.customer_id < 3;
explain analyze select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id and o.total_amount > 600 and o.status = 'ready' where c.customer_id < 3;
SQL
            ;;
        indexed_char_join_with_filter)
            cat > "${SQL_FILE}" <<'SQL'
create table users (user_id int, email char(20), lvl int);
create table profiles (profile_id int, email char(20), active int);
create index profiles(email);
insert into users values (1, 'a@x', 1);
insert into users values (2, 'b@x', 2);
insert into users values (3, 'c@x', 3);
insert into profiles values (101, 'a@x', 1);
insert into profiles values (102, 'b@x', 0);
insert into profiles values (103, 'c@x', 1);
select u.user_id, p.profile_id from users u join profiles p on u.email = p.email where u.lvl > 1 and p.active = 1;
explain analyze select u.user_id, p.profile_id from users u join profiles p on u.email = p.email where u.lvl > 1 and p.active = 1;
SQL
            ;;
        indexed_second_join_residual)
            cat > "${SQL_FILE}" <<'SQL'
create table customers (customer_id int, region int);
create table orders (order_id int, customer_id int, amount int);
create table shipments (ship_id int, order_id int, delayed int);
create index shipments(order_id);
insert into customers values (1, 1);
insert into customers values (2, 1);
insert into customers values (3, 2);
insert into orders values (101, 1, 500);
insert into orders values (102, 2, 900);
insert into orders values (103, 3, 700);
insert into shipments values (201, 101, 0);
insert into shipments values (202, 102, 1);
insert into shipments values (203, 103, 0);
select c.customer_id, o.order_id, s.ship_id from customers c join orders o on c.customer_id = o.customer_id join shipments s on o.order_id = s.order_id and s.delayed = 1 where c.region = 1;
explain analyze select c.customer_id, o.order_id, s.ship_id from customers c join orders o on c.customer_id = o.customer_id join shipments s on o.order_id = s.order_id and s.delayed = 1 where c.region = 1;
SQL
            ;;
        *)
            echo "unknown case: ${case_name}" >&2
            return 1
            ;;
    esac
}

run_suite() {
    local server="${1}"
    local client="${2}"
    local db_name="${3}"
    local server_log="${4}"
    local select_out="${5}"
    local explain_out="${6}"
    local client_err="${7}"
    local case_name="${8}"

    if ! wait_port_free "${SHARED_CLIENT}"; then
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

    write_case_sql "${case_name}"
    awk 'BEGIN{mode="setup"} /^select /{mode="select"} /^explain analyze /{mode="explain"} {print > ("'"${TMP_BASE}"'." mode ".sql")}' "${SQL_FILE}"

    "${client}" < "${TMP_BASE}.setup.sql" > /dev/null 2> "${client_err}"
    "${client}" < "${TMP_BASE}.select.sql" > "${select_out}" 2>> "${client_err}"
    "${client}" < "${TMP_BASE}.explain.sql" > "${explain_out}" 2>> "${client_err}"

    cleanup_server "${server_pid}"
    if ! wait_port_free "${SHARED_CLIENT}"; then
        echo "[FAIL] port 8765 did not become free after shutdown" >&2
        return 1
    fi
}

trap 'cleanup_server "${current_pid:-}"; cleanup_server "${ref_pid:-}"; cleanup_db "${CURRENT_DB}"; cleanup_db "${REF_DB}"; rm -rf "${CURRENT_LOG}" "${REF_LOG}" "${CURRENT_SELECT}" "${REF_SELECT}" "${CURRENT_EXPLAIN}" "${REF_EXPLAIN}" "${CURRENT_ERR}" "${REF_ERR}" "${SQL_FILE}" "${TMP_BASE}.setup.sql" "${TMP_BASE}.select.sql" "${TMP_BASE}.explain.sql"' EXIT

cases=(
    single_filter
    hidden_edges
    customer_orders
    three_join
    self_join_alias
    reversed_join_direction
    comma_join_right_filter
    wildcard_no_leaf_project
    propagated_eq_filter
    three_join_transitive_filter
    projection_ordering_alias
    composite_join_conditions
)

# explain_draft scope only: base filter/join/explain behavior plus plausible hidden
# variants around aliasing, predicate propagation, join orientation, and output order.
# Index-driven join cases are opt-in because they are useful hidden probes but may sit
# outside the strict minimum wording of the draft depending on interpretation.
if [[ "${INCLUDE_INDEX_CASES:-0}" == "1" ]]; then
    cases+=(
        indexed_right_join
        indexed_right_join_with_residual
        indexed_right_join_two_residuals
        indexed_char_join_with_filter
        indexed_second_join_residual
    )
fi

for case_name in "${cases[@]}"; do
    cases_run=$((cases_run + 1))
    echo "=== CASE: ${case_name} SELECT DIFF ==="
    run_suite "${CURRENT_SERVER}" "${CURRENT_CLIENT}" "${CURRENT_DB}" "${CURRENT_LOG}" "${CURRENT_SELECT}" "${CURRENT_EXPLAIN}" "${CURRENT_ERR}" "${case_name}"
    run_suite "${REF_SERVER}" "${SHARED_CLIENT}" "${REF_DB}" "${REF_LOG}" "${REF_SELECT}" "${REF_EXPLAIN}" "${REF_ERR}" "${case_name}"
    select_diff="$(diff -u "${REF_SELECT}" "${CURRENT_SELECT}" || true)"
    if [[ -n "${select_diff}" ]]; then
        failures=$((failures + 1))
        echo "${select_diff}"
    fi
    echo
    echo "=== CASE: ${case_name} EXPLAIN DIFF ==="
    explain_diff="$(diff -u "${REF_EXPLAIN}" "${CURRENT_EXPLAIN}" || true)"
    if [[ -n "${explain_diff}" ]]; then
        failures=$((failures + 1))
        echo "${explain_diff}"
    fi
    echo
done

if (( failures > 0 )); then
    echo "[FAIL] focused explain_draft parity found ${failures} diff block(s) across ${cases_run} case(s)" >&2
    exit 1
fi

echo "[PASS] focused explain_draft parity matched fullscore ref across ${cases_run} case(s)"
