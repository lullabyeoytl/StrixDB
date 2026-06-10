#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${ROOT_DIR}/build/bin/rmdb"
CLIENT="${ROOT_DIR}/build/bin/rmdb_client"
DB_PREFIX="index_task_case"
PORT="8765"

pass_count=0
fail_count=0

cleanup_server() {
    local pid="${1:-}"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
}

wait_port() {
    local tries=50
    while (( tries > 0 )); do
        if "${CLIENT}" < /dev/null >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        tries=$((tries - 1))
    done
    return 1
}

normalize_output() {
    sed -e 's/[[:space:]]*$//'
}

normalize_select_blocks() {
    awk '
        function trim(value) {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
            return value
        }

        function canonical_pipe_line(line,    parts, count, i, value, out) {
            count = split(line, parts, /\|/)
            out = ""
            for (i = 2; i < count; i++) {
                value = trim(parts[i])
                if (value == "") {
                    continue
                }
                out = out " | " value
            }
            if (out == "") {
                return ""
            }
            return out " |"
        }

        function flush_rows() {
            n = asorti(rows, sorted)
            for (i = 1; i <= n; i++) {
                count = rows[sorted[i]]
                for (j = 1; j <= count; j++) {
                    print sorted[i]
                }
            }
            delete rows
        }

        /^\+/ {
            next
        }

        /^Total record\(s\):/ {
            flush_rows()
            print
            next
        }

        /^\|/ {
            line = canonical_pipe_line($0)
            if (line == "| w_id | name |") {
                flush_rows()
                print line
                next
            }
            if (line != "") {
                rows[line]++
            }
            next
        }

        /^$/ {
            next
        }

        {
            flush_rows()
            rows[$0]++
        }

        END {
            flush_rows()
        }
    '
}

run_case() {
    local case_id="$1"
    local sql_file="$2"
    local expected_file="$3"
    local compare_mode="${4:-ordered}"
    local db_name="${DB_PREFIX}_${case_id}"
    local server_log="/tmp/${db_name}_server.log"

    case "${db_name}" in
        index_task_case_*) rm -rf "${ROOT_DIR:?}/${db_name}" ;;
        *) echo "unsafe database name: ${db_name}" >&2; exit 2 ;;
    esac

    "${SERVER}" "${db_name}" > "${server_log}" 2>&1 &
    local server_pid=$!
    trap "cleanup_server ${server_pid}" RETURN

    if ! wait_port; then
        echo "[FAIL] case ${case_id}: server did not accept connections"
        cat "${server_log}" || true
        fail_count=$((fail_count + 1))
        cleanup_server "${server_pid}"
        trap - RETURN
        return
    fi

    "${CLIENT}" < "${sql_file}" >/tmp/${db_name}_client.log 2>&1 || true

    cleanup_server "${server_pid}"
    trap - RETURN

    local actual_file="${ROOT_DIR}/${db_name}/output.txt"
    if [[ ! -f "${actual_file}" ]]; then
        actual_file="/dev/null"
    fi

    if [[ "${compare_mode}" == "unordered_select_rows" ]]; then
        if diff -u <(normalize_output < "${expected_file}" | normalize_select_blocks) <(normalize_output < "${actual_file}" | normalize_select_blocks) >/tmp/${db_name}_diff.log; then
            echo "[PASS] case ${case_id}"
            pass_count=$((pass_count + 1))
        else
            echo "[FAIL] case ${case_id}"
            cat /tmp/${db_name}_diff.log
            fail_count=$((fail_count + 1))
        fi
        return
    fi

    if diff -u <(normalize_output < "${expected_file}") <(normalize_output < "${actual_file}") >/tmp/${db_name}_diff.log; then
        echo "[PASS] case ${case_id}"
        pass_count=$((pass_count + 1))
    else
        echo "[FAIL] case ${case_id}"
        cat /tmp/${db_name}_diff.log
        fail_count=$((fail_count + 1))
    fi
}

tmp_dir="$(mktemp -d /tmp/index_task.XXXXXX)"
trap 'rm -rf "${tmp_dir}"' EXIT

cat > "${tmp_dir}/case1.sql" <<'SQL'
create table warehouse (id int, name char(8));
create index warehouse (id);
show index from warehouse;
create index warehouse (id,name);
show index from warehouse;
drop index warehouse (id);
drop index warehouse (id,name);
show index from warehouse;
SQL

cat > "${tmp_dir}/case1.expected" <<'EOF_EXPECTED'
| warehouse | unique | (id) |
| warehouse | unique | (id) |
| warehouse | unique | (id,name) |
EOF_EXPECTED

cat > "${tmp_dir}/case2.sql" <<'SQL'
create table warehouse (w_id int, name char(8));
insert into warehouse values (10 , 'qweruiop');
insert into warehouse values (534, 'asdfhjkl');
insert into warehouse values (100,'qwerghjk');
insert into warehouse values (500,'bgtyhnmj');
create index warehouse(w_id);
select * from warehouse where w_id = 10;
select * from warehouse where w_id < 534 and w_id > 100;
drop index warehouse(w_id);
create index warehouse(name);
select * from warehouse where name = 'qweruiop';
select * from warehouse where name > 'qwerghjk';
select * from warehouse where name > 'aszdefgh' and name < 'qweraaaa';
drop index warehouse(name);
create index warehouse(w_id,name);
select * from warehouse where w_id = 100 and name = 'qwerghjk';
select * from warehouse where w_id < 600 and name > 'bztyhnmj';
SQL

cat > "${tmp_dir}/case2.expected" <<'EOF_EXPECTED'
| w_id | name |
| 10 | qweruiop |
Total record(s): 1
| w_id | name |
| 500 | bgtyhnmj |
Total record(s): 1
| w_id | name |
| 10 | qweruiop |
Total record(s): 1
| w_id | name |
| 10 | qweruiop |
Total record(s): 1
| w_id | name |
| 500 | bgtyhnmj |
Total record(s): 1
| w_id | name |
| 100 | qwerghjk |
Total record(s): 1
| w_id | name |
| 10 | qweruiop |
| 100 | qwerghjk |
Total record(s): 2
EOF_EXPECTED

cat > "${tmp_dir}/case3.sql" <<'SQL'
create table warehouse (w_id int, name char(8));
insert into warehouse values (10 , 'qweruiop');
insert into warehouse values (534, 'asdfhjkl');
select * from warehouse where w_id = 10;
select * from warehouse where w_id < 534 and w_id > 100;
create index warehouse(w_id);
insert into warehouse values (500, 'lastdanc');
update warehouse set w_id = 507 where w_id = 534;
select * from warehouse where w_id = 10;
select * from warehouse where w_id < 534 and w_id > 100;
SQL

cat > "${tmp_dir}/case3.expected" <<'EOF_EXPECTED'
| w_id | name |
| 10 | qweruiop |
Total record(s): 1
| w_id | name |
Total record(s): 0
| w_id | name |
| 10 | qweruiop |
Total record(s): 1
| w_id | name |
| 500 | lastdanc |
| 507 | asdfhjkl |
Total record(s): 2
EOF_EXPECTED

run_case 1 "${tmp_dir}/case1.sql" "${tmp_dir}/case1.expected"
run_case 2 "${tmp_dir}/case2.sql" "${tmp_dir}/case2.expected" unordered_select_rows
run_case 3 "${tmp_dir}/case3.sql" "${tmp_dir}/case3.expected" unordered_select_rows

echo "summary: pass=${pass_count}, fail=${fail_count}"
if (( fail_count > 0 )); then
    exit 1
fi
