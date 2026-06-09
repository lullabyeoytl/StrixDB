#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${PROJECT_ROOT}/build/bin/rmdb"
CLIENT="${PROJECT_ROOT}/build/bin/rmdb_client"

cleanup_server() {
    local pid="${1:-}"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
}

wait_port() {
    local tries=80
    while (( tries > 0 )); do
        if "${CLIENT}" < /dev/null >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        tries=$((tries - 1))
    done
    return 1
}

wait_port_free() {
    local tries=80
    while (( tries > 0 )); do
        if ! "${CLIENT}" < /dev/null >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        tries=$((tries - 1))
    done
    return 1
}

assert_contains() {
    local output="$1"
    local expected="$2"
    if [[ "${output}" != *"${expected}"* ]]; then
        echo "[FAIL] missing expected output:" >&2
        echo "${expected}" >&2
        failures=$((failures + 1))
    fi
}

assert_not_contains() {
    local output="$1"
    local unexpected="$2"
    if [[ "${output}" == *"${unexpected}"* ]]; then
        echo "[FAIL] unexpected output:" >&2
        echo "${unexpected}" >&2
        failures=$((failures + 1))
    fi
}

db_name="strixdb_explain_regression_$$"
server_log="/tmp/${db_name}.server.log"
setup_out="/tmp/${db_name}.setup.out"
select_out="/tmp/${db_name}.select.out"
explain_out="/tmp/${db_name}.explain.out"
client_err="/tmp/${db_name}.client.err"
failures=0

cd /tmp
if ! wait_port_free; then
    echo "[FAIL] server port did not become free" >&2
    exit 1
fi

"${SERVER}" "${db_name}" > "${server_log}" 2>&1 &
server_pid=$!
trap 'cleanup_server "${server_pid}"; rm -rf "/tmp/${db_name}" "${server_log}" "${setup_out}" "${select_out}" "${explain_out}" "${client_err}"' EXIT

if ! wait_port; then
    echo "[FAIL] server did not accept connections" >&2
    cat "${server_log}" >&2 || true
    exit 1
fi

cat <<'SQL' | "${CLIENT}" > "${setup_out}" 2> "${client_err}"
create table t (a int, b int);
insert into t values (1, 5);
insert into t values (2, 8);
insert into t values (3, 12);
insert into t values (4, 6);
insert into t values (5, 20);
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
create table shipments (ship_id int, order_id int, status char(16));
insert into shipments values (501, 101, 'packed');
insert into shipments values (502, 102, 'ready');
insert into shipments values (503, 103, 'sent');
insert into shipments values (504, 104, 'held');
create table t469 (id int, grp int, score int, name char(8));
insert into t469 values (1, 10, 80, 'a');
insert into t469 values (2, 10, 60, 'b');
insert into t469 values (3, 20, 90, 'c');
SQL

if [[ -s "${client_err}" ]]; then
    cat "${client_err}" >&2
    exit 1
fi

cat <<'SQL' | "${CLIENT}" > "${select_out}" 2> "${client_err}"
select a, b from t where a > 1 and b < 10;
select * from customers c join orders o on c.customer_id = o.customer_id where o.total_amount > 1000;
select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id;
select c.name, o.order_id, s.ship_id from customers c join orders o on c.customer_id = o.customer_id join shipments s on o.order_id = s.order_id where c.customer_id < 3 and s.ship_id > 501;
select l.name, r.name from t469 l join t469 r on l.grp = r.grp where l.score > 70 and r.score < 70;
SQL

select_output="$(cat "${select_out}")"
if [[ -s "${client_err}" ]]; then
    cat "${client_err}" >&2
    exit 1
fi

cat <<'SQL' | "${CLIENT}" > "${explain_out}" 2> "${client_err}"
explain analyze select a, b from t where a > 1 and b < 10;
explain analyze select * from customers c join orders o on c.customer_id = o.customer_id where o.total_amount > 1000;
explain analyze select c.name, o.order_id from customers c join orders o on c.customer_id = o.customer_id;
explain analyze select c.name, o.order_id, s.ship_id from customers c join orders o on c.customer_id = o.customer_id join shipments s on o.order_id = s.order_id where c.customer_id < 3 and s.ship_id > 501;
explain analyze select l.name, r.name from t469 l join t469 r on l.grp = r.grp where l.score > 70 and r.score < 70;
SQL

explain_output="$(cat "${explain_out}")"
if [[ -s "${client_err}" ]]; then
    cat "${client_err}" >&2
    exit 1
fi

assert_contains "${select_output}" "|                2 |                8 |"
assert_contains "${select_output}" "|                4 |                6 |"
assert_contains "${select_output}" "Alice"
assert_contains "${select_output}" "Bob"
assert_contains "${select_output}" "Carol"
assert_contains "${select_output}" "|            Alice |              102 |              502 |"
assert_contains "${select_output}" "|              Bob |              103 |              503 |"
assert_contains "${select_output}" "|                a |                b |"
assert_not_contains "${select_output}" "Error:"

assert_contains "${explain_output}" "Project(columns=[t.a, t.b], rows=2)"
assert_contains "${explain_output}" $'\tFilter(condition=[t.a>1, t.b<10], rows=2)'
assert_contains "${explain_output}" $'\t\tScan(table=t, type=SeqScan, rows=5)'
assert_contains "${explain_output}" "Project(columns=[*], rows=2)"
assert_contains "${explain_output}" $'\tJoin(tables=[customers, orders], condition=[c.customer_id=o.customer_id], rows=2)'
assert_contains "${explain_output}" $'\t\tScan(table=customers, type=SeqScan, rows=3)'
assert_contains "${explain_output}" $'\t\tFilter(condition=[o.total_amount>1000], rows=6)'
assert_contains "${explain_output}" $'\t\t\tScan(table=orders, type=SeqScan, rows=15)'
assert_contains "${explain_output}" "Project(columns=[c.name, o.order_id], rows=5)"
assert_contains "${explain_output}" $'\tJoin(tables=[customers, orders], condition=[c.customer_id=o.customer_id], rows=5)'
assert_contains "${explain_output}" $'\t\tProject(columns=[c.customer_id, c.name], rows=3)'
assert_contains "${explain_output}" $'\t\t\tScan(table=customers, type=SeqScan, rows=3)'
assert_contains "${explain_output}" $'\t\tProject(columns=[o.customer_id, o.order_id], rows=15)'
assert_contains "${explain_output}" $'\t\t\tScan(table=orders, type=SeqScan, rows=15)'
assert_contains "${explain_output}" "Project(columns=[c.name, o.order_id, s.ship_id], rows=3)"
assert_contains "${explain_output}" $'\tJoin(tables=[customers, orders, shipments], condition=[o.order_id=s.order_id], rows=3)'
assert_contains "${explain_output}" $'\t\tJoin(tables=[customers, orders], condition=[c.customer_id=o.customer_id], rows=4)'
assert_contains "${explain_output}" $'\t\t\tProject(columns=[c.customer_id, c.name], rows=2)'
assert_contains "${explain_output}" $'\t\t\t\tFilter(condition=[c.customer_id<3], rows=2)'
assert_contains "${explain_output}" $'\t\t\t\t\tScan(table=customers, type=SeqScan, rows=3)'
assert_contains "${explain_output}" $'\t\t\tProject(columns=[o.customer_id, o.order_id], rows=8)'
assert_contains "${explain_output}" $'\t\t\t\tFilter(condition=[o.customer_id<3], rows=8)'
assert_contains "${explain_output}" $'\t\t\t\t\tScan(table=orders, type=SeqScan, rows=10)'
assert_contains "${explain_output}" $'\t\tProject(columns=[s.order_id, s.ship_id], rows=12)'
assert_contains "${explain_output}" $'\t\t\tFilter(condition=[s.ship_id>501], rows=12)'
assert_contains "${explain_output}" $'\t\t\t\tScan(table=shipments, type=SeqScan, rows=16)'
assert_contains "${explain_output}" "Project(columns=[l.name, r.name], rows=1)"
assert_contains "${explain_output}" $'\tJoin(tables=[t469], condition=[l.grp=r.grp], rows=1)'
assert_contains "${explain_output}" $'\t\tProject(columns=[l.grp, l.name], rows=2)'
assert_contains "${explain_output}" $'\t\t\tFilter(condition=[l.score>70], rows=2)'
assert_contains "${explain_output}" $'\t\t\t\tScan(table=t469, type=SeqScan, rows=3)'
assert_contains "${explain_output}" $'\t\tProject(columns=[r.grp, r.name], rows=2)'
assert_contains "${explain_output}" $'\t\t\tFilter(condition=[r.score<70], rows=2)'
assert_contains "${explain_output}" $'\t\t\t\tScan(table=t469, type=SeqScan, rows=6)'
assert_not_contains "${explain_output}" "Total record(s):"
assert_not_contains "${explain_output}" "Error:"

if (( failures > 0 )); then
    echo "[FAIL] explain regression alignment (${failures} assertions failed)" >&2
    exit 1
fi

echo "[PASS] explain regression alignment"
