#!/bin/bash
# StrixDB 全量 SQL 测试 — 单文件入口
#
# 用法:
#   ./test/run_all_tests.sh              # 运行全部测试
#   ./test/run_all_tests.sh basic        # 仅基础查询
#   ./test/run_all_tests.sh dml          # 仅 DML
#   ./test/run_all_tests.sh aggregate             # 仅聚合
#   ./test/run_all_tests.sh unique                # 仅唯一索引
#   ./test/run_all_tests.sh access_path           # 仅访问路径选择
#   ./test/run_all_tests.sh dml_index_scan        # 仅 DML 索引扫描
#   ./test/run_all_tests.sh cross_type_predicate  # 仅跨类型数值谓词
#   ./test/run_all_tests.sh transaction           # 仅事务/恢复
#   ./test/run_all_tests.sh datetime              # 仅 DATETIME 类型
#   ./test/run_all_tests.sh explain_analyze       # 仅 EXPLAIN ANALYZE
#   ./test/run_all_tests.sh covering_index        # 仅覆盖索引扫描
#   ./test/run_all_tests.sh static_checkpoint     # 仅静态检查点恢复
#
# 新增测试: 在 test_all.sql 中按 @@section 格式添加 SQL 节，
# 然后在 TEST_FUNCTIONS 数组中注册测试函数即可。

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SQL_FILE="${SCRIPT_DIR}/test_all.sql"

SERVER="${PROJECT_ROOT}/build/bin/rmdb"
CLIENT="${PROJECT_ROOT}/build/bin/rmdb_client"

# ── 框架函数 ──────────────────────────────────────────────

PASS=0
FAIL=0
SERVER_PID=""
DB_NAME=""
SERVER_LOG=""

framework_init() {
    DB_NAME="$1"
    SERVER_LOG="/tmp/rmdb_${DB_NAME}_$$.log"
    PASS=0
    FAIL=0

    echo "=== [framework] 清理旧数据库 ==="
    rm -rf "$DB_NAME"

    echo "=== [framework] 启动 server (后台) ==="
    "$SERVER" "$DB_NAME" > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 1

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "ERROR: Server 启动失败"
        cat "$SERVER_LOG"
        exit 1
    fi
    echo "  Server PID=$SERVER_PID"
}

framework_cleanup() {
    echo ""
    echo "=== [framework] 清理 ==="
    if [ -n "$SERVER_PID" ]; then
        echo "  Killing server PID=$SERVER_PID" >&2
        kill "$SERVER_PID" 2>/dev/null || true
        # 等待进程退出，最多 5 秒
        local _waited=0
        while kill -0 "$SERVER_PID" 2>/dev/null; do
            sleep 0.5
            _waited=$((_waited + 1))
            if [ $_waited -ge 10 ]; then
                kill -9 "$SERVER_PID" 2>/dev/null || true
                sleep 0.5
                break
            fi
        done
    fi
    if [ -n "$DB_NAME" ]; then
        rm -rf "$DB_NAME"
    fi
    rm -f "$SERVER_LOG"
}

run_sql() {
    local sql="$1"
    echo "  SQL: $sql" >&2
    local out
    out=$(echo "$sql" | "$CLIENT" 2>/dev/null || true)
    echo "$out" >&2
    echo "  ---" >&2
    echo "$out"
}

send_sql() {
    local sql="$1"
    echo "  SQL: $sql" >&2
    echo "$sql" | "$CLIENT" 2>/dev/null >/dev/null || true
}

check_contains() {
    local desc="$1" output="$2" pattern="$3"
    if echo "$output" | grep -q "$pattern"; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc — '$pattern' 未找到"
        FAIL=$((FAIL + 1))
    fi
}

check_contains_literal() {
    local desc="$1" output="$2" pattern="$3"
    if echo "$output" | grep -Fq "$pattern"; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc — '$pattern' 未找到"
        FAIL=$((FAIL + 1))
    fi
}

check_not_contains() {
    local desc="$1" output="$2" pattern="$3"
    if echo "$output" | grep -q "$pattern"; then
        echo "  [FAIL] $desc — 不应出现 '$pattern'"
        FAIL=$((FAIL + 1))
    else
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    fi
}

check_row_count() {
    local desc="$1" output="$2" expected="$3"
    local actual
    actual=$(echo "$output" | grep -oP 'Total record\(s\): \K\d+' || echo "0")
    if [ "$actual" -eq "$expected" ]; then
        echo "  [PASS] $desc (rows=$actual)"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc — 期望 $expected 行, 实际 $actual 行"
        FAIL=$((FAIL + 1))
    fi
}

check_count() {
    local desc="$1" output="$2" pattern="$3" expected="$4"
    local actual
    actual=$(echo "$output" | grep -E '^\| ' | grep -v -E '^\| *[a-zA-Z]' | grep -c "$pattern" || true)
    if [ "$actual" -eq "$expected" ]; then
        echo "  [PASS] $desc (count=$actual)"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc — 期望 count=$expected, 实际 $actual"
        FAIL=$((FAIL + 1))
    fi
}

check_line_order() {
    local desc="$1" output="$2" first_pattern="$3" second_pattern="$4"
    local first_line second_line
    first_line=$(echo "$output" | grep -nE "$first_pattern" | head -n 1 | cut -d: -f1)
    second_line=$(echo "$output" | grep -nE "$second_pattern" | head -n 1 | cut -d: -f1)
    if [ -n "$first_line" ] && [ -n "$second_line" ] && [ "$first_line" -lt "$second_line" ]; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc — 行位置不符合预期"
        FAIL=$((FAIL + 1))
    fi
}

check_not_contains_regex() {
    local desc="$1" output="$2" pattern="$3"
    if echo "$output" | grep -Eq "$pattern"; then
        echo "  [FAIL] $desc — 不应出现正则 '$pattern'"
        FAIL=$((FAIL + 1))
    else
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    fi
}

framework_summary() {
    echo ""
    echo "============================================"
    echo "  结果: 通过 $PASS, 失败 $FAIL"
    echo "============================================"
    if [ "$FAIL" -gt 0 ]; then
        return 1
    fi
    return 0
}

# ── 等待端口释放 ─────────────────────────────────────────

wait_port_free() {
    local max_wait=15
    local waited=0
    local total_waited=0
    while fuser 8765/tcp >/dev/null 2>&1; do
        if [ $waited -ge $max_wait ]; then
            fuser -k 8765/tcp 2>/dev/null || true
            sleep 1
            waited=0
            max_wait=25
        fi
        sleep 1
        waited=$((waited + 1))
        total_waited=$((total_waited + 1))
        if [ $total_waited -ge 60 ]; then
            echo "WARNING: Port 8765 still occupied after 60s, proceeding anyway"
            fuser -k 8765/tcp 2>/dev/null || true
            sleep 2
            break
        fi
    done
}

# ── 测试节辅助 ───────────────────────────────────────────

test_section_header() {
    echo ""
    echo "╔════════════════════════════════════════════════╗"
    echo "║  $1"
    echo "╚════════════════════════════════════════════════╝"
}

# ── SQL 文件节提取与执行 ─────────────────────────────────

# 从 test_all.sql 中提取指定节的所有 SQL（去掉标记行和注释）
extract_section() {
    local section="$1"
    awk "/^-- @@section ${section}$/{flag=1; next} /^-- @@endsection$/{flag=0} flag" "$SQL_FILE"
}

# 逐条执行节内 SQL，SELECT 语句的输出收集到命名数组中
# 用法: execute_section_stmts <section> <result_array_name>
execute_section_stmts() {
    local section="$1"
    local -n _results="$2"

    local stmt=""
    local line
    while IFS= read -r line; do
        # 跳过空行和注释行
        [[ -z "$line" ]] && continue
        [[ "$line" =~ ^[[:space:]]*-- ]] && continue

        # 累积语句直到分号结尾
        if [[ -n "$stmt" ]]; then
            stmt+=$'\n'
        fi
        stmt+="$line"

        if [[ "$line" =~ \;$ ]]; then
            echo "  SQL: ${stmt:0:80}..." >&2
            # 判断是否为查询语句，查询结果需捕获用于断言
            if [[ "${stmt,,}" =~ ^[[:space:]]*select ]]; then
                local out
                out=$(echo "$stmt" | "$CLIENT" 2>/dev/null || true)
                echo "$out" >&2
                echo "  ---" >&2
                _results+=("$out")
            else
                echo "$stmt" | "$CLIENT" 2>/dev/null >/dev/null || true
            fi
            stmt=""
        fi
    done < <(extract_section "$section")
}

execute_explain_section_stmts() {
    local section="$1"
    local -n _results="$2"

    local stmt=""
    local line
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        [[ "$line" =~ ^[[:space:]]*-- ]] && continue

        if [[ -n "$stmt" ]]; then
            stmt+=$'\n'
        fi
        stmt+="$line"

        if [[ "$line" =~ \;$ ]]; then
            echo "  SQL: ${stmt:0:80}..." >&2
            if [[ "${stmt,,}" =~ ^[[:space:]]*(select|explain[[:space:]]+analyze) ]]; then
                local out
                out=$(echo "$stmt" | "$CLIENT" 2>/dev/null || true)
                echo "$out" >&2
                echo "  ---" >&2
                _results+=("$out")
            else
                echo "$stmt" | "$CLIENT" 2>/dev/null >/dev/null || true
            fi
            stmt=""
        fi
    done < <(extract_section "$section")
}

# 执行包含事务 (begin/commit/rollback) 的 SQL 节
# - 独立语句逐条发送（每连接一条）
# - begin; ... commit;/rollback; 组通过同一连接原子发送
# - SELECT 输出收集到命名数组中
execute_transaction_section() {
    local section="$1"
    local -n _txn_results="$2"

    local stmt="" line txn_buf="" in_txn=false trimmed

    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        [[ "$line" =~ ^[[:space:]]*-- ]] && continue

        if [[ -n "$stmt" ]]; then stmt+=$'\n'; fi
        stmt+="$line"

        if [[ "$line" =~ \;$ ]]; then
            trimmed="${stmt##[[:space:]]}"
            trimmed="${trimmed%%[[:space:]]}"

            if [[ "${trimmed,,}" =~ ^begin[[:space:]]*\; ]]; then
                in_txn=true
                txn_buf="$stmt"$'\n'
            elif $in_txn; then
                txn_buf+="$stmt"$'\n'
                if [[ "${trimmed,,}" =~ ^(commit|rollback)[[:space:]]*\; ]]; then
                    echo "  TXN: ${txn_buf:0:80}..." >&2
                    local txn_out
                    txn_out=$(printf '%s' "$txn_buf" | "$CLIENT" 2>/dev/null || true)
                    echo "$txn_out" >&2
                    echo "  ---" >&2
                    _txn_results+=("$txn_out")
                    in_txn=false
                    txn_buf=""
                fi
            else
                echo "  SQL: ${stmt:0:80}..." >&2
                if [[ "${trimmed,,}" =~ ^select ]]; then
                    local out
                    out=$(echo "$stmt" | "$CLIENT" 2>/dev/null || true)
                    echo "$out" >&2
                    echo "  ---" >&2
                    _txn_results+=("$out")
                else
                    echo "$stmt" | "$CLIENT" 2>/dev/null >/dev/null || true
                fi
            fi
            stmt=""
        fi
    done < <(extract_section "$section")
}

execute_session_section() {
    local section="$1"
    local -n _session_results="$2"

    local script=""
    local line
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        [[ "$line" =~ ^[[:space:]]*-- ]] && continue
        script+="$line"$'\n'
    done < <(extract_section "$section")

    local output
    output=$(printf '%s' "$script" | "$CLIENT" 2>/dev/null || true)

    local current=""
    while IFS= read -r line; do
        if [[ -z "$current" ]]; then
            current="$line"
        else
            current+=$'\n'"$line"
        fi

        if [[ "$line" =~ ^Total[[:space:]]record\(s\):[[:space:]] ]]; then
            _session_results+=("$current")
            current=""
        fi
    done <<< "$output"
}

execute_interleaved_section() {
    local section="$1"
    local summary_file
    summary_file="$(mktemp /tmp/rmdb_interleaved.XXXXXX)"

    if python3 - "$SQL_FILE" "$section" "$summary_file" <<'PY'
import socket
import sys
import time

sql_file = sys.argv[1]
section = sys.argv[2]
summary_file = sys.argv[3]

pass_count = 0
fail_count = 0


class Client:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", 8765), timeout=5)
        self.sock.settimeout(5)

    def sql(self, query):
        self.sock.sendall(query.encode() + b"\0")
        chunks = []
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            if b"\0" in chunk:
                before, _ = chunk.split(b"\0", 1)
                chunks.append(before)
                break
            chunks.append(chunk)
        return b"".join(chunks).decode(errors="replace")

    def close(self):
        self.sock.close()


def write_summary():
    with open(summary_file, "w", encoding="utf-8") as file:
        file.write(f"PASS_DELTA={pass_count}\n")
        file.write(f"FAIL_DELTA={fail_count}\n")


def check(label, ok, detail, output):
    global pass_count, fail_count
    if ok:
        print(f"  [PASS] {label}")
        pass_count += 1
        return
    print(f"  [FAIL] {label} -- {detail}")
    if output:
        print(output)
    fail_count += 1


def section_lines():
    flag = False
    start = f"-- @@section {section}"
    with open(sql_file, encoding="utf-8") as file:
        for raw in file:
            line = raw.rstrip("\n")
            if line == start:
                flag = True
                continue
            if line == "-- @@endsection" and flag:
                return
            if flag:
                yield line
    raise RuntimeError(f"section not found: {section}")


clients = {}
current_session = "default"
pending_label = ""
pending_expectations = []
stmt = []


def get_client(name):
    client = clients.get(name)
    if client is None:
        client = Client()
        clients[name] = client
    return client


def close_client(name):
    client = clients.pop(name, None)
    if client is not None:
        client.close()


def parse_expectation(body):
    if body.startswith("not_contains "):
        return ("not_contains", body[len("not_contains "):])
    if body.startswith("contains "):
        return ("contains", body[len("contains "):])
    if body.startswith("row_count "):
        return ("row_count", body[len("row_count "):])
    raise RuntimeError(f"unsupported expectation: {body}")


def flush_statement():
    global pending_label, pending_expectations, stmt
    if not stmt:
        return
    query = "\n".join(stmt)
    label = pending_label or query.replace("\n", " ")[:80]
    print(f"  SQL[{current_session}]: {query.replace(chr(10), ' ')[:100]}...")
    output = get_client(current_session).sql(query)
    if output:
        print(output)
        print("  ---")
    for kind, value in pending_expectations:
        if kind == "contains":
            check(label, value in output, f"missing {value!r}", output)
        elif kind == "not_contains":
            check(label, value not in output, f"unexpected {value!r}", output)
        elif kind == "row_count":
            expected = f"Total record(s): {value}"
            check(label, expected in output, f"missing {expected!r}", output)
    pending_label = ""
    pending_expectations = []
    stmt = []


try:
    for line in section_lines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("-- @session "):
            flush_statement()
            current_session = stripped[len("-- @session "):].strip()
            continue
        if stripped.startswith("-- @close "):
            flush_statement()
            close_client(stripped[len("-- @close "):].strip())
            continue
        if stripped.startswith("-- @sleep "):
            flush_statement()
            time.sleep(float(stripped[len("-- @sleep "):].strip()))
            continue
        if stripped.startswith("-- @label "):
            pending_label = stripped[len("-- @label "):].strip()
            continue
        if stripped.startswith("-- @expect "):
            pending_expectations.append(parse_expectation(stripped[len("-- @expect "):]))
            continue
        if stripped.startswith("--"):
            continue
        stmt.append(line)
        if stripped.endswith(";"):
            flush_statement()
except Exception as exc:
    check(section, False, str(exc), "")
finally:
    for client in list(clients.values()):
        try:
            client.close()
        except OSError:
            pass
    write_summary()

sys.exit(1 if fail_count else 0)
PY
    then
        # shellcheck disable=SC1090
        source "$summary_file"
        PASS=$((PASS + PASS_DELTA))
        FAIL=$((FAIL + FAIL_DELTA))
        rm -f "$summary_file"
        return 0
    fi

    if [ -f "$summary_file" ]; then
        # shellcheck disable=SC1090
        source "$summary_file"
        PASS=$((PASS + PASS_DELTA))
        FAIL=$((FAIL + FAIL_DELTA))
    else
        FAIL=$((FAIL + 1))
    fi
    rm -f "$summary_file"
    return 0
}

# ═══════════════════════════════════════════════════════════
#  测试节: 基础查询与 WHERE 过滤
# ═══════════════════════════════════════════════════════════

test_basic() {
    wait_port_free
    framework_init "test_basic"
    test_section_header "基础查询与 WHERE 过滤"

    local -a R
    execute_section_stmts "basic" R

    # R[0]: select id, name from t;                        → 3 行
    check_contains "row alice"   "${R[0]}" "alice"
    check_contains "row bravo"   "${R[0]}" "bravo"
    check_contains "row charlie" "${R[0]}" "charlie"

    # R[1]: select id, name from t where id = 1;           → alice
    check_contains "int eq" "${R[1]}" "alice"

    # R[2]: select name from t where name = 'bravo';       → bravo
    check_contains "string eq" "${R[2]}" "bravo"

    # R[3]: select name from t where score = 3.5;          → charlie
    check_contains "float eq" "${R[3]}" "charlie"

    # R[4]: select id from t where id = 999;               → 0 行
    check_row_count "no match returns 0 rows" "${R[4]}" 0

    # R[5]: select id, name from t where id > 2;           → charlie, delta
    check_contains "gt filter charlie" "${R[5]}" "charlie"
    check_contains "gt filter delta"   "${R[5]}" "delta"

    # R[6]: select id, name from t where id >= 2 and score < 3.0; → bravo
    check_contains "multi condition" "${R[6]}" "bravo"

    # R[7]: select a, b from t2 where a = b;               → (1,1),(3,3)
    check_contains "col eq 1-1" "${R[7]}" "|                1 |                1 |"
    check_contains "col eq 3-3" "${R[7]}" "|                3 |                3 |"

    # R[8]: select a, b from t2 where a > b;               → 0 行
    check_row_count "col gt returns 0 rows" "${R[8]}" 0

    # R[9]: select a, b from t2 where a < b;               → (2,3),(4,7)
    check_contains "col lt 2-3" "${R[9]}" "|                2 |                3 |"
    check_contains "col lt 4-7" "${R[9]}" "|                4 |                7 |"

    framework_cleanup
    echo ""
    echo ">>> 基础查询测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: DML 操作 (INSERT/DELETE/UPDATE)
# ═══════════════════════════════════════════════════════════

test_dml() {
    wait_port_free
    framework_init "test_dml"
    test_section_header "DML 操作 (INSERT/DELETE/UPDATE)"

    local -a R
    execute_section_stmts "dml" R

    # R[0]: select id, name from t;                        → 4 行初始数据
    check_contains "alice"   "${R[0]}" "alice"
    check_contains "bravo"   "${R[0]}" "bravo"
    check_contains "charlie" "${R[0]}" "charlie"
    check_contains "delta"   "${R[0]}" "delta"

    # R[1]: after delete id=1 → alice 消失
    check_not_contains "alice deleted" "${R[1]}" "alice"
    check_contains "bravo survives"    "${R[1]}" "bravo"

    # R[2]: after delete name='bravo' → bravo 消失
    check_not_contains "bravo deleted" "${R[2]}" "bravo"

    # R[3]: after delete all → 0 行
    check_not_contains "all deleted" "${R[3]}" "charlie|delta"

    # R[4]: after update score=100 where id=1
    check_contains "update score"  "${R[4]}" "100"
    check_contains "name preserved" "${R[4]}" "alice"

    # R[5]: where score=100.0 after update
    check_contains "filter after update" "${R[5]}" "alice"

    # R[6]: after multi-row update name='updated'
    check_count "two rows updated" "${R[6]}" "updated" 2

    # R[7]: mixed ops (insert, update, delete) → id>=3
    check_contains "mixed ops updated" "${R[7]}" "updated"
    check_contains "mixed ops changed" "${R[7]}" "changed"
    check_not_contains "deleted row absent" "${R[7]}" "|                2 "

    # R[8]: select from ti where a=2 (index scan)
    check_contains "index scan" "${R[8]}" "2"
    check_contains "index scan val" "${R[8]}" "20"

    # R[9]: after index delete a=2
    check_not_contains "index delete" "${R[9]}" "|                2 "

    # R[10]: after index update a=100 where a=1
    check_contains "index update" "${R[10]}" "100"

    framework_cleanup
    echo ""
    echo ">>> DML 测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 聚合函数与 GROUP BY/HAVING
# ═══════════════════════════════════════════════════════════

test_aggregate() {
    wait_port_free
    framework_init "test_aggregate"
    test_section_header "聚合函数与 GROUP BY/HAVING"

    local -a R
    execute_section_stmts "aggregate" R

    # R[0]: count(*), sum, avg, min, max
    check_contains "count(*)"    "${R[0]}" "3"
    check_contains "sum(salary)" "${R[0]}" "60"
    check_contains "avg(salary)" "${R[0]}" "20"
    check_contains "min(salary)" "${R[0]}" "10"
    check_contains "max(salary)" "${R[0]}" "30"

    # R[1]: GROUP BY dept
    check_contains "group dept=1" "${R[1]}" "|                1 |                2 |               40 |"
    check_contains "group dept=2" "${R[1]}" "|                2 |                1 |               20 |"

    # R[2]: HAVING count(*) > 1
    check_contains "having count" "${R[2]}" "|                1 |                2 |"
    check_not_contains_regex "filters dept=2" "${R[2]}" "\\|[[:space:]]*2[[:space:]]*\\|[[:space:]]*1[[:space:]]*\\|"

    # R[3]: HAVING dept=2
    check_contains "having dept=2" "${R[3]}" "|                2 |                1 |"

    # R[4]: 非聚合列未在 GROUP BY → 应报错
    check_contains "selection rule error" "${R[4]}" "GROUP BY"

    framework_cleanup
    echo ""
    echo ">>> 聚合测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 唯一索引
# ═══════════════════════════════════════════════════════════

test_unique() {
    wait_port_free
    framework_init "test_unique"
    test_section_header "唯一索引"

    local -a R
    execute_section_stmts "unique" R

    # R[0]: select from tu1 (2 rows, dup rejected)
    check_contains "row 1" "${R[0]}" "1"
    check_contains "row 2" "${R[0]}" "2"
    check_not_contains "no dup" "${R[0]}" "300"

    # R[1]: select from tu2 (composite index, 3 rows)
    check_count "3 rows remain" "${R[1]}" "|.*|" 3

    # R[2]: select from tu3 (single index, 2 rows)
    check_contains "row 10" "${R[2]}" "10"
    check_contains "row 20" "${R[2]}" "20"
    check_not_contains "no 9.9" "${R[2]}" "9.9"

    # R[3]: select from tu4 (dup data, index creation should fail)
    check_contains "data unchanged 10" "${R[3]}" "10"
    check_contains "data unchanged 20" "${R[3]}" "20"

    # R[4]: select from tu5 (update to existing key rejected)
    check_contains "data unchanged 1-10" "${R[4]}" "|                1 |               10 |"
    check_contains "data unchanged 2-20" "${R[4]}" "|                2 |               20 |"

    # R[5]: select from tu5 where id=1 (self-update ok)
    check_row_count "self-hit row" "${R[5]}" 1

    # R[6]: select from tu6 (default unique index rejects duplicates)
    check_contains "dup 100" "${R[6]}" "100"
    check_not_contains "no dup 200" "${R[6]}" "200"
    check_not_contains "no dup 300" "${R[6]}" "300"

    # R[7]: select from tu7 (batch update keeps unique keys)
    check_row_count "batch unchanged unique keys keep two rows" "${R[7]}" 2
    check_contains "batch row 1 updated" "${R[7]}" "|                1 |               99 |"
    check_contains "batch row 2 updated" "${R[7]}" "|                2 |               99 |"

    # R[8]: select from tu8 (batch duplicate unique key update rejected)
    check_row_count "batch duplicate key update rejected" "${R[8]}" 2
    check_contains "batch duplicate keeps row 1" "${R[8]}" "|                1 |               10 |"
    check_contains "batch duplicate keeps row 2" "${R[8]}" "|                2 |               20 |"
    check_not_contains "batch duplicate does not create target key" "${R[8]}" "|                9 |"

    # R[9]: select from tu9 (batch move to distinct composite unique keys)
    check_row_count "batch composite unique move keeps two rows" "${R[9]}" 2
    check_contains "batch composite move row 10" "${R[9]}" "|                2 |               10 |"
    check_contains "batch composite move row 20" "${R[9]}" "|                2 |               20 |"

    # R[10]: select from tu10 (visible duplicate unique update rejected)
    check_row_count "visible duplicate unique update rejected" "${R[10]}" 2
    check_contains "visible duplicate keeps row 1" "${R[10]}" "|                1 |               10 |"
    check_contains "visible duplicate keeps row 2" "${R[10]}" "|                2 |               20 |"

    framework_cleanup
    echo ""
    echo ">>> 唯一索引测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 访问路径选择
# ═══════════════════════════════════════════════════════════

test_access_path() {
    wait_port_free
    framework_init "test_access_path"
    test_section_header "访问路径选择 (Access Path)"

    local -a R
    execute_section_stmts "access_path" R

    # ── Group 1: 单列索引等值查询 ──
    # R[0]: select where id=3
    check_row_count "index scan returns 1 row" "${R[0]}" 1
    check_contains "value 30 present" "${R[0]}" "30"

    # R[1]: select where id=999
    check_row_count "no matching row" "${R[1]}" 0

    # R[2]: select where val=20 (non-indexed)
    check_row_count "non-indexed filter 1 row" "${R[2]}" 1
    check_contains "value 2 present" "${R[2]}" "2"

    # ── Group 2: 复合索引前导列匹配 ──
    # R[3]: where a=1
    check_row_count "leading col returns 2 rows" "${R[3]}" 2

    # R[4]: where a=1 and b=20
    check_row_count "exact composite match" "${R[4]}" 1
    check_contains "has 200" "${R[4]}" "200"

    # R[5]: where b=10 (non-leading)
    check_row_count "non-leading col fallback" "${R[5]}" 3

    # R[6]: where a=2 and d=3000 (residual filter)
    check_row_count "residual filter 1 row" "${R[6]}" 1
    check_contains "has 3000" "${R[6]}" "3000"

    # ── Group 3: 等价推断与谓词规范化 ──
    # R[7]: where a=b and b=5
    check_row_count "equiv const inferred" "${R[7]}" 1
    check_contains "row a=5,b=5,c=10" "${R[7]}" "10"

    # R[8]: where a=5 and a=5 (dedup)
    check_row_count "dedup returns 2 rows" "${R[8]}" 2

    # R[9]: where a=5 and a=6 (contradiction)
    check_row_count "contradiction yields empty" "${R[9]}" 0

    # R[10]: where a=b and b=5 and c>5
    check_row_count "equiv + range, 1 row" "${R[10]}" 1
    check_contains "c=10 returned" "${R[10]}" "10"

    # R[11]: where a=b and b=8
    check_row_count "equiv chain 1 row" "${R[11]}" 1
    check_contains "c=30 returned" "${R[11]}" "30"

    # ── Group 4: 范围条件识别 ──
    # R[12]: where x > 15
    check_row_count "x > 15" "${R[12]}" 5

    # R[13]: where x >= 15
    check_row_count "x >= 15" "${R[13]}" 6

    # R[14]: where x < 5
    check_row_count "x < 5" "${R[14]}" 4

    # R[15]: where x <= 5
    check_row_count "x <= 5" "${R[15]}" 5

    # R[16]: where x >= 5 and x <= 8
    check_row_count "range intersection" "${R[16]}" 4

    # R[17]: where x > 10 and x < 11
    check_row_count "empty range" "${R[17]}" 0

    # ── Group 5: 索引选择策略 ──
    # R[18]: where a=1 and b=20 (longer prefix)
    check_row_count "longer eq prefix" "${R[18]}" 1
    check_contains "result has 200"  "${R[18]}" "200"

    # R[19]: where a=2 and c>250 (range)
    check_row_count "range preferred" "${R[19]}" 2

    # R[20]: where d=1000 (seqscan fallback)
    check_row_count "seqscan fallback" "${R[20]}" 1

    # ── Group 6: 大量数据正确性 ──
    # R[21]: where k1=5
    check_row_count "large table, k1=5" "${R[21]}" 10

    # R[22]: where k1=5 and k2=1
    check_row_count "exact match" "${R[22]}" 1

    # R[23]: where k1 > 8
    check_row_count "range scan" "${R[23]}" 21

    # ── Group 7: DML 访问路径 ──
    # R[24]: select where id=99 after insert
    check_row_count "row exists" "${R[24]}" 1

    # R[25]: select where id=99 after delete
    check_row_count "row deleted" "${R[25]}" 0

    # R[26]: select where id=1 after update
    check_row_count "update target still queryable" "${R[26]}" 1
    check_contains "update target id preserved" "${R[26]}" "|                1 |"

    # R[27]: contradiction delete preserved
    check_row_count "contradiction delete preserved" "${R[27]}" 1

    # R[28]: dedup update where id=3
    check_row_count "dedup update target still queryable" "${R[28]}" 1
    check_contains "dedup update target id preserved" "${R[28]}" "|                3 |"

    framework_cleanup
    echo ""
    echo ">>> 访问路径测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: DML 通过索引扫描的执行路径
# ═══════════════════════════════════════════════════════════

test_dml_index_scan() {
    wait_port_free
    framework_init "test_dml_index_scan"
    test_section_header "DML 通过 IndexScan 子执行器 (batch 路径)"

    local -a R
    execute_section_stmts "dml_index_scan" R

    # R[0]: select where id=5 (initial verify)
    check_row_count "index scan finds id=5" "${R[0]}" 1
    check_contains "val is 50" "${R[0]}" "50"

    # R[1]: select where id=5 after delete
    check_row_count "deleted row gone" "${R[1]}" 0

    # R[2]: select where id=4 (neighbor survives)
    check_row_count "neighbor survives after index delete" "${R[2]}" 1

    # R[3]: select where id>=15 after range delete
    check_row_count "range delete cleared" "${R[3]}" 0

    # R[4]: select where id=14 (boundary survives)
    check_row_count "boundary neighbor survives" "${R[4]}" 1

    # R[5]: select where id=3 after single update
    check_row_count "single row update target queryable" "${R[5]}" 1
    check_contains "single row update target id preserved" "${R[5]}" "|                3 |"

    # R[6]: select where id>=10 and id<=12 after range update
    check_row_count "range update target set size" "${R[6]}" 3
    check_contains "range update includes id=10" "${R[6]}" "|               10 |"
    check_contains "range update includes id=11" "${R[6]}" "|               11 |"
    check_contains "range update includes id=12" "${R[6]}" "|               12 |"

    # R[7]: select where id=1 (untouched row)
    check_row_count "untouched row 1" "${R[7]}" 1
    check_contains "val unchanged" "${R[7]}" "10"

    framework_cleanup
    echo ""
    echo ">>> DML 索引扫描路径测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 跨类型数值谓词
# ═══════════════════════════════════════════════════════════

test_cross_type_predicate() {
    wait_port_free
    framework_init "test_cross_type_predicate"
    test_section_header "跨类型数值谓词 (int vs float)"

    local -a R
    execute_section_stmts "cross_type" R

    # R[0]: where id = 2.0
    check_row_count "int = float eq" "${R[0]}" 1
    check_contains "id=2" "${R[0]}" "|                2 |"

    # R[1]: where id > 1.5
    check_row_count "int > float" "${R[1]}" 2
    check_contains "id=2 present" "${R[1]}" "|                2 |"
    check_contains "id=3 present" "${R[1]}" "|                3 |"
    check_not_contains "id=1 excluded" "${R[1]}" "|                1 |"

    # R[2]: where id < 3.0
    check_row_count "int < float" "${R[2]}" 2
    check_contains "id=1 present" "${R[2]}" "|                1 |"
    check_contains "id=2 present" "${R[2]}" "|                2 |"

    # R[3]: where id >= 2.0
    check_row_count "int >= float" "${R[3]}" 2

    # R[4]: where id <= 2.0
    check_row_count "int <= float" "${R[4]}" 2

    # R[5]: where id = 2 (int = int regression)
    check_row_count "int = int" "${R[5]}" 1
    check_contains "id=2 int" "${R[5]}" "|                2 |"

    # R[6]: where id > 1.5 with index
    check_row_count "indexed int > float" "${R[6]}" 2

    framework_cleanup
    echo ""
    echo ">>> 跨类型谓词测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 事务提交、回滚与崩溃恢复
# ═══════════════════════════════════════════════════════════

# 将多条 SQL 经同一客户端连接发送
txn_send() {
    local combined=""
    for stmt in "$@"; do
        combined+="${stmt}"$'\n'
    done
    printf '%s' "$combined" | "$CLIENT" 2>/dev/null >/dev/null || true
}

# 同上，但捕获并返回服务端输出
txn_run() {
    local combined=""
    for stmt in "$@"; do
        combined+="${stmt}"$'\n'
    done
    printf '%s' "$combined" | "$CLIENT" 2>/dev/null || true
}

# 发送 crash 命令终止服务器并等待端口释放
crash_server() {
    echo "crash" | "$CLIENT" 2>/dev/null || true
    local waited=0
    while fuser 8765/tcp >/dev/null 2>&1; do
        sleep 1
        waited=$((waited + 1))
        if [ $waited -ge 8 ]; then
            fuser -k 8765/tcp 2>/dev/null || true
            sleep 1
            break
        fi
    done
}

# 在同一 DB_NAME 上重启服务器
restart_server() {
    "$SERVER" "$DB_NAME" > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 2
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "  ERROR: server failed to restart"
        cat "$SERVER_LOG"
        return 1
    fi
}

test_transaction() {
    wait_port_free
    framework_init "test_transaction"
    test_section_header "事务：提交、回滚与崩溃恢复"

    # ── 执行事务节（begin/commit/rollback 组原子发送）──
    echo ""
    echo "━━━ 事务提交/回滚测试 ━━━"
    local -a R
    execute_transaction_section "transaction" R

    # 结果数组: txn 组输出与 SELECT 输出交替
    # R[0]: begin+insert delta+commit 组输出
    # R[1]: select id, name from t where id = 4
    check_contains "committed insert visible" "${R[1]}" "delta"

    # R[2]: begin+insert echo+rollback 组输出
    # R[3]: select id, name from t where id = 5
    check_row_count "rolled-back insert absent" "${R[3]}" 0

    # R[4]: begin+update modified+rollback 组输出
    # R[5]: select id, name from t where id = 1
    check_contains  "original value restored" "${R[5]}" "alice"
    check_not_contains "modified value absent"   "${R[5]}" "modified"

    # R[6]: begin+delete id=2+rollback 组输出
    # R[7]: select id, name from t where id = 2
    check_row_count "rolled-back delete: row restored"  "${R[7]}" 1
    check_contains  "rolled-back delete: value correct" "${R[7]}" "bravo"

    # R[8]: begin+multi dml+rollback 组输出
    # R[9]: select id from t where id = 6
    check_row_count "multi-txn rollback: id=6 absent" "${R[9]}" 0
    # R[10]: select id from t where id = 7
    check_row_count "multi-txn rollback: id=7 absent" "${R[10]}" 0
    # R[11]: select id, name from t where id = 3
    check_contains "multi-txn rollback: charlie restored" "${R[11]}" "charlie"

    # R[12]: begin+insert ti 3+rollback 组输出
    # R[13]: select id, val from ti where id = 3
    check_row_count "index table: rolled-back insert absent" "${R[13]}" 0

    # R[14]: begin+update ti val=999+commit 组输出
    # R[15]: select id, val from ti where id = 1
    check_row_count "index table: committed update target queryable" "${R[15]}" 1
    check_contains "index table: committed update key preserved" "${R[15]}" "|                1 |"

    # R[16]: begin+update ti val=0+rollback 组输出
    # R[17]: select id, val from ti where id = 1
    check_row_count "index table: rolled-back update target queryable" "${R[17]}" 1
    check_contains "index table: rolled-back update key preserved" "${R[17]}" "|                1 |"

    # ── Group 7: 崩溃恢复 — 已提交数据持久化 ──
    echo ""
    echo "━━━ 崩溃恢复: 已提交数据持久化 ━━━"
    # 从 test_all.sql 提取崩溃恢复数据准备节
    local -a _crash_setup
    execute_section_stmts "transaction_crash" _crash_setup

    crash_server

    if restart_server; then
        RESULT=$(run_sql "select id, name from t_crash where id = 10;")
        check_row_count "crash-recovery: committed row survives" "$RESULT" 1
        check_contains  "crash-recovery: row value correct"      "$RESULT" "survivor"

        RESULT=$(run_sql "select id, name from t_crash;")
        check_contains "crash-recovery: table intact" "$RESULT" "10"
    else
        FAIL=$((FAIL + 1))
    fi

    # ── Group 7b: 崩溃恢复 — 未提交事务回滚 ──
    echo ""
    echo "━━━ 崩溃恢复: 未提交事务回滚 ━━━"
    printf '%s\n' "begin;" \
                  "insert into t_crash values (20, 200, 'ghost');" \
                  "crash" | "$CLIENT" 2>/dev/null || true

    local waited=0
    while fuser 8765/tcp >/dev/null 2>&1; do
        sleep 1; waited=$((waited + 1))
        [ $waited -ge 8 ] && { fuser -k 8765/tcp 2>/dev/null || true; sleep 1; break; }
    done

    if restart_server; then
        RESULT=$(run_sql "select id, name from t_crash where id = 20;")
        check_row_count "crash-recovery: uncommitted row absent" "$RESULT" 0
    else
        FAIL=$((FAIL + 1))
    fi

    # ── Group 7c: 崩溃恢复 — 未提交 DELETE 回滚 ──
    echo ""
    echo "━━━ 崩溃恢复: 未提交 DELETE 回滚 ━━━"

    printf '%s\n' "begin;" \
                  "delete from t_crash where id = 30;" \
                  "crash" | "$CLIENT" 2>/dev/null || true

    waited=0
    while fuser 8765/tcp >/dev/null 2>&1; do
        sleep 1; waited=$((waited + 1))
        [ $waited -ge 8 ] && { fuser -k 8765/tcp 2>/dev/null || true; sleep 1; break; }
    done

    if restart_server; then
        RESULT=$(run_sql "select id, name from t_crash where id = 30;")
        check_row_count "crash-recovery: uncommitted delete restored" "$RESULT" 1
        check_contains  "crash-recovery: deleted value correct"      "$RESULT" "doomed"
    else
        FAIL=$((FAIL + 1))
    fi

    # ── Group 7d: 崩溃恢复 — 未提交 UPDATE 回滚 ──
    echo ""
    echo "━━━ 崩溃恢复: 未提交 UPDATE 回滚 ━━━"
    printf '%s\n' "begin;" \
                  "update t_crash set name = 'changed' where id = 10;" \
                  "crash" | "$CLIENT" 2>/dev/null || true

    waited=0
    while fuser 8765/tcp >/dev/null 2>&1; do
        sleep 1; waited=$((waited + 1))
        [ $waited -ge 8 ] && { fuser -k 8765/tcp 2>/dev/null || true; sleep 1; break; }
    done

    if restart_server; then
        RESULT=$(run_sql "select id, name from t_crash where id = 10;")
        check_contains "crash-recovery: uncommitted update reverted" "$RESULT" "survivor"
        check_not_contains "crash-recovery: modified value absent"    "$RESULT" "changed"
    else
        FAIL=$((FAIL + 1))
    fi

    send_sql "drop table t_crash;"

    framework_cleanup
    echo ""
    echo ">>> 事务测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 排序 (ORDER BY)
# ═══════════════════════════════════════════════════════════

test_sort() {
    wait_port_free
    framework_init "test_sort"
    test_section_header "排序 (ORDER BY)"

    local -a R
    execute_section_stmts "sort" R

    # R[0]: ORDER BY id ASC → (1,10), (2,20), (3,30)
    check_row_count "sort: asc row count" "${R[0]}" 3
    check_contains "sort: first id=1 with val=10" "${R[0]}" "|                1 |               10 |"
    check_contains "sort: last id=3 with val=30"  "${R[0]}" "|                3 |               30 |"

    # R[1]: ORDER BY id DESC → (3,30), (2,20), (1,10)
    check_row_count "sort: desc row count" "${R[1]}" 3

    # R[2]: ORDER BY name ASC → alpha, bravo, charlie
    check_row_count "sort: string sort row count" "${R[2]}" 3
    check_contains "sort: alpha first" "${R[2]}" "alpha"
    check_contains "sort: charlie last" "${R[2]}" "charlie"

    # R[3]: Without ORDER BY, just verify all rows present
    check_row_count "sort: unsorted has all rows" "${R[3]}" 3

    framework_cleanup
    echo ""
    echo ">>> 排序测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 连接 (JOIN)
# ═══════════════════════════════════════════════════════════

test_join() {
    wait_port_free
    framework_init "test_join"
    test_section_header "连接 (JOIN)"

    local -a R
    execute_section_stmts "join" R

    # R[0]: INNER JOIN left_t.lid = right_t.rid
    # left: (1,10), (2,20), (3,30)  right: (1,100), (2,200), (2,250)
    # Matches: (1,10,100), (2,20,200), (2,20,250) = 3 rows
    check_row_count "join: inner equi-join row count" "${R[0]}" 3

    # R[1]: SEMI JOIN → only left rows that match: (1,10), (2,20) = 2 rows
    check_row_count "join: semi join row count" "${R[1]}" 2
    check_not_contains "join: semi join excludes id=3" "${R[1]}" "|                3 |"

    # R[2]: Join with lval >= 20 → (2,20,200), (2,20,250), (3,30,?) = no match for 3
    # Actually left(3,30) has lid=3, right has no rid=3, so only left(2,20):
    # (2,20,200) and (2,20,250) = 2 rows
    check_row_count "join: filtered join row count" "${R[2]}" 2

    # R[3]: CROSS JOIN → 3 left * 3 right = 9 rows
    check_row_count "join: cross join row count" "${R[3]}" 9

    # R[4]: No matching rows → 0 rows
    check_row_count "join: no-match row count" "${R[4]}" 0

    framework_cleanup
    echo ""
    echo ">>> 连接测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 连接 + 排序
# ═══════════════════════════════════════════════════════════

test_join_sort() {
    wait_port_free
    framework_init "test_join_sort"
    test_section_header "连接 + 排序 (JOIN + ORDER BY)"

    local -a R
    execute_section_stmts "join_sort" R

    # emp: (alice,eng,300), (bob,sales,500), (carol,eng,400)
    # dept: (eng,engineering), (sales,sales)
    # Matches: alice→eng(300), bob→sales(500), carol→eng(400)
    # R[0]: ORDER BY salary DESC → bob(500), carol(400), alice(300)
    check_row_count "join_sort: join+sort row count" "${R[0]}" 3
    check_contains "join_sort: bob highest salary" "${R[0]}" "bob"

    # R[1]: ORDER BY name ASC → alice, bob, carol
    check_row_count "join_sort: join+sort asc row count" "${R[1]}" 3
    check_contains "join_sort: alice first" "${R[1]}" "alice"

    framework_cleanup
    echo ""
    echo ">>> 连接+排序测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 聚合 + 排序 (GROUP BY + ORDER BY)
# ═══════════════════════════════════════════════════════════

test_aggregate_sort() {
    wait_port_free
    framework_init "test_aggregate_sort"
    test_section_header "聚合 + 排序 (GROUP BY + ORDER BY)"

    local -a R
    execute_section_stmts "aggregate_sort" R

    # R[0]: GROUP BY dept ORDER BY dept ASC (sort aggregation path)
    # Expect: dept=1(2行,sum=40), dept=2(2行,sum=70), dept=3(1行,sum=40) — 3 rows
    check_row_count "agg_sort: asc row count" "${R[0]}" 3
    check_contains "agg_sort: dept=1 sum=40" "${R[0]}" "1.*2.*40"
    check_contains "agg_sort: dept=2 sum=70" "${R[0]}" "2.*2.*70"
    check_contains "agg_sort: dept=3 sum=40" "${R[0]}" "3.*1.*40"

    # R[1]: GROUP BY dept ORDER BY dept DESC
    check_row_count "agg_sort: desc row count" "${R[1]}" 3
    check_contains "agg_sort: dept desc first row" "${R[1]}" "|                3 |                1 |               40 |"

    # R[2]: GROUP BY dept ORDER BY sum(salary) DESC
    check_row_count "agg_sort: aggregate expression row count" "${R[2]}" 3
    check_contains "agg_sort: aggregate expression highest sum" "${R[2]}" "|                2 |                2 |               70 |"

    # R[3]: Multi-column GROUP BY dept,role ORDER BY dept,role ASC (sort aggregation)
    check_row_count "agg_sort: multi-key row count" "${R[3]}" 4
    check_contains "agg_sort: multi-key dept=1 role=10" "${R[3]}" "1.*10.*2.*300"
    check_contains "agg_sort: multi-key dept=2 role=20" "${R[3]}" "2.*20.*1.*500"

    framework_cleanup
    echo ""
    echo ">>> 聚合+排序测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: DATETIME 类型
# ═══════════════════════════════════════════════════════════

test_datetime() {
    wait_port_free
    framework_init "test_datetime"
    test_section_header "DATETIME 类型"

    local -a R
    execute_section_stmts "datetime" R

    check_row_count "datetime: order row count" "${R[0]}" 3
    check_contains "datetime: earliest visible" "${R[0]}" "2024-01-01 00:00:00"
    check_contains "datetime: middle visible" "${R[0]}" "2024-01-02 03:04:05"
    check_contains "datetime: latest visible" "${R[0]}" "2024-01-03 12:30:00"

    check_row_count "datetime: equality row count" "${R[1]}" 1
    check_contains "datetime: equality finds id=1" "${R[1]}" "|                1 |"

    check_row_count "datetime: range row count" "${R[2]}" 2
    check_contains "datetime: range includes early" "${R[2]}" "2024-01-01 00:00:00"
    check_contains "datetime: range includes middle" "${R[2]}" "2024-01-02 03:04:05"

    check_row_count "datetime: update row count" "${R[3]}" 1
    check_contains "datetime: update visible" "${R[3]}" "2024-01-04 00:00:00"

    check_row_count "datetime: min max row count" "${R[4]}" 1
    check_contains "datetime: min visible" "${R[4]}" "2024-01-01 00:00:00"
    check_contains "datetime: max visible" "${R[4]}" "2024-01-04 00:00:00"

    check_row_count "datetime: index equality row count" "${R[5]}" 1
    check_contains "datetime: index equality id=3" "${R[5]}" "|                3 |"

    check_row_count "datetime: index range row count" "${R[6]}" 2
    check_contains "datetime: index range includes id=1" "${R[6]}" "|                1 |"
    check_contains "datetime: index range includes id=3" "${R[6]}" "|                3 |"

    check_row_count "datetime: composite index row count" "${R[7]}" 1
    check_contains "datetime: composite index id=1" "${R[7]}" "|                1 |"

    check_row_count "datetime: indexed table row count" "${R[8]}" 2
    check_contains "datetime: indexed table first" "${R[8]}" "2024-02-01 00:00:00"
    check_contains "datetime: indexed table second" "${R[8]}" "2024-02-02 00:00:00"

    send_sql "create table dt_bad (id int, created_at datetime);"
    local invalid_out
    invalid_out=$(run_sql "insert into dt_bad values (1, '2023-02-29 00:00:00');")
    check_contains "datetime: invalid date rejected" "$invalid_out" "Invalid datetime literal"
    send_sql "drop table dt_bad;"

    send_sql "create table dtu_bad (id int, created_at datetime);"
    send_sql "create index dtu_bad(created_at);"
    send_sql "insert into dtu_bad values (1, '2024-03-01 00:00:00');"
    local unique_out
    unique_out=$(run_sql "insert into dtu_bad values (2, '2024-03-01 00:00:00');")
    check_contains "datetime: indexed duplicate rejected" "$unique_out" "Unique violation"
    send_sql "drop table dtu_bad;"

    framework_cleanup
    echo ""
    echo ">>> DATETIME 类型测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: EXPLAIN ANALYZE
# ═══════════════════════════════════════════════════════════

test_explain_analyze() {
    wait_port_free
    framework_init "test_explain_analyze"
    test_section_header "EXPLAIN ANALYZE"

    local -a R
    execute_explain_section_stmts "explain_analyze" R

    check_row_count "plain select still returns rows" "${R[0]}" 2
    check_contains_literal "single table project" "${R[1]}" "Project(columns=[t.a, t.b], rows=2)"
    check_contains_literal "single table filter" "${R[1]}" "Filter(condition=[t.a>1, t.b<10], rows=2)"
    check_contains_literal "single table scan type" "${R[1]}" "Scan(table=t, type=SeqScan, rows=5)"
    check_not_contains "single table no result table" "${R[1]}" "Total record(s)"
    check_not_contains "single table no tabular output" "${R[1]}" "| customer_id |"

    check_contains_literal "join project wildcard" "${R[2]}" "Project(columns=[*], rows=2)"
    check_contains_literal "join predicate" "${R[2]}" "Join(tables=[customers, orders], condition=[c.customer_id=o.customer_id], rows=2)"
    check_contains_literal "join pushed filter" "${R[2]}" "Filter(condition=[o.total_amount>1000], rows=2)"
    check_contains_literal "join customer scan" "${R[2]}" "Scan(table=customers, type=SeqScan, rows=3)"
    check_contains_literal "join order scan" "${R[2]}" "Scan(table=orders, type=SeqScan, rows=5)"

    check_contains_literal "projection root" "${R[3]}" "Project(columns=[c.name, o.order_id], rows=5)"
    check_contains_literal "left projection pushdown" "${R[3]}" "Project(columns=[c.customer_id, c.name], rows=3)"
    check_contains_literal "right projection pushdown" "${R[3]}" "Project(columns=[o.customer_id, o.order_id], rows=5)"
    check_not_contains "projection trims customer email" "${R[3]}" "c.email"
    check_not_contains "projection trims order amount" "${R[3]}" "o.total_amount"

    framework_cleanup
    echo ""
    echo ">>> EXPLAIN ANALYZE 测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: 覆盖索引扫描
# ═══════════════════════════════════════════════════════════

run_covering_mvcc_snapshot_case() {
    CLIENT="$CLIENT" python3 <<'PY'
import socket
import sys


class Client:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", 8765), timeout=5)
        self.sock.settimeout(5)

    def sql(self, query):
        self.sock.sendall(query.encode() + b"\0")
        chunks = []
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            if b"\0" in chunk:
                before, _ = chunk.split(b"\0", 1)
                chunks.append(before)
                break
            chunks.append(chunk)
        return b"".join(chunks).decode(errors="replace")

    def close(self):
        self.sock.close()


def check(label, condition, detail=""):
    if condition:
        print(f"  [PASS] {label}")
    else:
        print(f"  [FAIL] {label} {detail}")
        global failures
        failures += 1


failures = 0
clients = []
try:
    setup = Client()
    reader = Client()
    writer = Client()
    new_reader = Client()
    clients.extend([setup, reader, writer, new_reader])

    setup.sql("create table ci_mv (id int, code int, payload char(8));")
    setup.sql("insert into ci_mv values (10, 100, 'old');")
    setup.sql("create index ci_mv(id, code);")

    reader.sql("set transaction isolation level snapshot isolation;")
    writer.sql("set transaction isolation level snapshot isolation;")
    reader.sql("begin;")
    initial = reader.sql("select id, code from ci_mv where id = 10;")
    check("ambiguous covering reader registers table before update", "Total record(s): 1" in initial)
    writer.sql("begin;")
    writer.sql("update ci_mv set id = 11 where id = 10;")
    writer.sql("commit;")

    old_snapshot = reader.sql("select id, code from ci_mv where id = 10;")
    check("ambiguous covering old snapshot returns old key", "Total record(s): 1" in old_snapshot)
    check("ambiguous covering old snapshot value", "|               10 |              100 |" in old_snapshot)

    old_payload = reader.sql("select id, payload from ci_mv where id = 10;")
    check("ambiguous non-covering old snapshot returns old key", "Total record(s): 1" in old_payload)
    check("ambiguous non-covering old snapshot value", "old" in old_payload)
    reader.sql("commit;")

    current = new_reader.sql("select id, code from ci_mv where id = 11;")
    check("new reader returns current covered key", "Total record(s): 1" in current)
    check("new reader current covered value", "|               11 |              100 |" in current)
finally:
    for client in clients:
        try:
            client.close()
        except OSError:
            pass

sys.exit(1 if failures else 0)
PY
}

test_covering_index() {
    wait_port_free
    framework_init "test_covering_index"
    test_section_header "覆盖索引扫描"

    send_sql "create table ci (id int, code int, payload char(8));"
    send_sql "insert into ci values (1, 10, 'alpha');"
    send_sql "insert into ci values (2, 20, 'bravo');"
    send_sql "insert into ci values (3, 30, 'charlie');"
    send_sql "insert into ci values (4, 40, 'delta');"
    send_sql "insert into ci values (5, 50, 'echo');"
    send_sql "create index ci(id, code);"

    local covered_eq
    covered_eq=$(run_sql "select id, code from ci where id = 2;")
    check_row_count "covering equality returns one row" "$covered_eq" 1
    check_contains "covering equality returns indexed columns" "$covered_eq" "|                2 |               20 |"

    local covered_eq_plan
    covered_eq_plan=$(run_sql "explain analyze select id, code from ci where id = 2;")
    check_contains_literal "covering equality uses index scan" "$covered_eq_plan" "Scan(table=ci, type=IndexScan"

    local covered_range
    covered_range=$(run_sql "select id, code from ci where id >= 2 and id <= 4;")
    check_row_count "covering range returns three rows" "$covered_range" 3
    check_contains "covering range includes lower row" "$covered_range" "|                2 |               20 |"
    check_contains "covering range includes middle row" "$covered_range" "|                3 |               30 |"
    check_contains "covering range includes upper row" "$covered_range" "|                4 |               40 |"

    local raw_heap
    raw_heap=$(run_sql "select id, payload from ci where id = 2;")
    check_row_count "non-covering equality returns one row" "$raw_heap" 1
    check_contains "non-covering equality returns payload" "$raw_heap" "bravo"

    local covered_residual
    covered_residual=$(run_sql "select id, code from ci where id >= 2 and code = 20;")
    check_row_count "covering residual returns one row" "$covered_residual" 1
    check_contains "covering residual returns indexed columns" "$covered_residual" "|                2 |               20 |"

    if run_covering_mvcc_snapshot_case; then
        PASS=$((PASS + 6))
    else
        FAIL=$((FAIL + 1))
    fi

    framework_cleanup
    echo ""
    echo ">>> 覆盖索引扫描测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试节: MVCC/SSI 串行 SQL 能力
# ═══════════════════════════════════════════════════════════

test_mvcc_ssi_sql() {
    wait_port_free
    framework_init "test_mvcc_ssi_sql"
    test_section_header "MVCC/SSI 串行 SQL 能力"

    local -a R
    execute_session_section "mvcc_ssi_sql" R

    check_row_count "snapshot transaction sees own insert" "${R[0]}" 1
    check_contains "snapshot transaction own insert value" "${R[0]}" "|                2 |              200 |"

    check_row_count "snapshot committed insert visible" "${R[1]}" 1
    check_contains "snapshot committed insert value" "${R[1]}" "|                2 |              200 |"

    check_row_count "serializable transaction reads base row" "${R[2]}" 1
    check_contains "serializable transaction value" "${R[2]}" "|                1 |              100 |"

    check_row_count "snapshot transaction sees own update" "${R[3]}" 1
    check_contains "snapshot transaction own update value" "${R[3]}" "|                1 |               20 |"

    check_row_count "snapshot rollback restores old value" "${R[4]}" 1
    check_contains "snapshot rollback value" "${R[4]}" "|                1 |               10 |"

    framework_cleanup
    echo ""
    echo ">>> MVCC/SSI 串行 SQL 能力测试完成"
    framework_summary
}

test_mvcc_task6_9() {
    wait_port_free
    framework_init "test_mvcc_task6_9"
    test_section_header "MVCC task6-9 snapshot visibility"
    execute_interleaved_section "mvcc_task6_9"
    framework_cleanup
    echo ""
    echo ">>> MVCC task6-9 snapshot visibility 测试完成"
    framework_summary
}

test_mvcc_gc() {
    wait_port_free
    framework_init "test_mvcc_gc"
    test_section_header "MVCC long reader table-local GC"
    execute_interleaved_section "mvcc_gc"
    framework_cleanup
    echo ""
    echo ">>> MVCC long reader table-local GC 测试完成"
    framework_summary
}

test_mvcc_si() {
    wait_port_free
    framework_init "test_mvcc_si"
    test_section_header "MVCC snapshot isolation concurrent checks"
    execute_interleaved_section "mvcc_si"
    framework_cleanup
    echo ""
    echo ">>> MVCC snapshot isolation concurrent checks 测试完成"
    framework_summary
}

test_mvcc_ssi_draft() {
    wait_port_free
    test_section_header "MVCC SSI draft examples"
    bash "$SCRIPT_DIR/check_mvcc_ssi_draft.sh"
}

test_mvcc_ser() {
    wait_port_free
    framework_init "test_mvcc_ser"
    test_section_header "MVCC serializable SSI concurrent checks"
    execute_interleaved_section "mvcc_ser"
    framework_cleanup
    echo ""
    echo ">>> MVCC serializable SSI concurrent checks 测试完成"
    framework_summary
}

test_mvcc_ssi_coverage() {
    wait_port_free
    framework_init "test_mvcc_ssi_coverage"
    test_section_header "MVCC SSI coverage matrix"
    execute_interleaved_section "mvcc_ssi_coverage"
    framework_cleanup
    echo ""
    echo ">>> MVCC SSI coverage matrix 测试完成"
    framework_summary
}

test_mvcc_client_failure() {
    wait_port_free
    test_section_header "MVCC client external failure handling"

    local output
    local status
    set +e
    output=$(printf '%s\n' "select * from missing_server;" | "$CLIENT" -p 18765 2>&1)
    status=$?
    set -e

    if [ "$status" -ne 0 ]; then
        echo "  [PASS] client returns nonzero when DB server is unavailable"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] client returns nonzero when DB server is unavailable"
        FAIL=$((FAIL + 1))
    fi

    if echo "$output" | grep -Eq "create socket error|Failed to connect|failed to connect"; then
        echo "  [PASS] client reports connection failure"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] client reports connection failure"
        echo "$output"
        FAIL=$((FAIL + 1))
    fi

    echo ""
    echo ">>> MVCC client external failure handling 测试完成"
    framework_summary
}

test_static_checkpoint() {
    wait_port_free
    test_section_header "静态检查点恢复"
    timeout 180s bash "$SCRIPT_DIR/check_static_checkpoint.sh"
}

test_union() {
    wait_port_free
    framework_init "test_union"
    test_section_header "并集查询"

    local -a R
    execute_section_stmts "union" R

    check_row_count "union: two branches deduplicate full rows" "${R[0]}" 4
    check_contains "union: promoted duplicate row once" "${R[0]}" "1.000000.*10.*aa"
    check_contains "union: same id different name survives" "${R[0]}" "2.000000.*20.*cc"
    check_contains "union: longer char branch preserved" "${R[0]}" "charlie"
    check_count "union: duplicate row count" "${R[0]}" "1.000000.*aa" 1
    check_line_order "union: two branch order applies after union" "${R[0]}" "1.000000.*10.*aa" "3.000000.*30.*charlie"

    check_row_count "union: three branches row count" "${R[1]}" 5
    check_contains "union: multi-key keeps score 30 first group" "${R[1]}" "3.000000.*30.*charlie"
    check_contains "union: third branch row visible" "${R[1]}" "4.000000.*20.*ee"
    check_line_order "union: multi-key order applies after union" "${R[1]}" "3.000000.*30.*charlie" "1.000000.*10.*aa"

    check_row_count "union: default order row count" "${R[2]}" 4
    check_contains "union: default direction has lowest id" "${R[2]}" "1.000000.*10.*aa"

    local missing_col
    missing_col=$(run_sql "select * from (select * from u_num_a union select * from u_num_b) as u order by label;")
    check_contains "union: unknown output sort key rejected" "$missing_col" "Column not found"

    local count_mismatch
    count_mismatch=$(run_sql "select * from (select id from u_num_a union select * from u_num_b) as u;")
    check_contains "union: branch column count mismatch rejected" "$count_mismatch" "same number of columns"

    local type_mismatch
    type_mismatch=$(run_sql "select * from (select name from u_num_a union select xid from u_num_b) as u;")
    check_contains "union: incompatible branch type rejected" "$type_mismatch" "Incompatible type error"

    send_sql "drop table u_num_c;"
    send_sql "drop table u_num_b;"
    send_sql "drop table u_num_a;"

    framework_cleanup
    echo ""
    echo ">>> 并集查询测试完成"
    framework_summary
}

# ═══════════════════════════════════════════════════════════
#  测试函数注册表
# ═══════════════════════════════════════════════════════════

# 格式: "函数名:显示名称"
TEST_FUNCTIONS=(
    "test_basic:基础查询与 WHERE 过滤"
    "test_dml:DML 操作 (INSERT/DELETE/UPDATE)"
    "test_aggregate:聚合函数与 GROUP BY/HAVING"
    "test_unique:唯一索引"
    "test_access_path:访问路径选择"
    "test_dml_index_scan:DML 通过索引扫描子执行器"
    "test_cross_type_predicate:跨类型数值谓词"
    "test_transaction:事务提交、回滚与崩溃恢复"
    "test_sort:排序 (ORDER BY)"
    "test_join:连接 (JOIN)"
    "test_join_sort:连接 + 排序"
    "test_aggregate_sort:聚合 + 排序 (GROUP BY + ORDER BY)"
    "test_union:并集查询"
    "test_datetime:DATETIME 类型"
    "test_explain_analyze:EXPLAIN ANALYZE"
    "test_covering_index:覆盖索引扫描"
    "test_mvcc_ssi_sql:MVCC/SSI 串行 SQL 能力"
    "test_mvcc_task6_9:MVCC 快照可见性"
    "test_mvcc_gc:MVCC 长读者表级回收"
    "test_mvcc_si:MVCC 快照隔离并发能力"
    "test_mvcc_ssi_draft:MVCC/SSI 参考场景"
    "test_mvcc_ser:MVCC 可串行化并发能力"
    "test_mvcc_ssi_coverage:MVCC/SSI 覆盖矩阵"
    "test_mvcc_client_failure:MVCC 客户端外部故障"
    "test_static_checkpoint:静态检查点恢复"
)

# ── 颜色 ─────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

# ═══════════════════════════════════════════════════════════
#  主入口
# ═══════════════════════════════════════════════════════════

FILTER="${1:-}"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║         StrixDB 全量 SQL 测试套件                         ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

declare -a SUITE_RESULTS

for entry in "${TEST_FUNCTIONS[@]}"; do
    func="${entry%%:*}"
    name="${entry##*:}"

    if [ -n "$FILTER" ] && [ "${func#test_}" != "$FILTER" ]; then
        continue
    fi

    echo ""
    echo "################################################################"
    echo "#  运行: $name"
    echo "################################################################"

    if "$func"; then
        echo -e "${GREEN}[PASS]${NC} $name"
        SUITE_RESULTS+=("PASS")
    else
        echo -e "${RED}[FAIL]${NC} $name"
        SUITE_RESULTS+=("FAIL")
    fi
done

# 汇总
echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║         全量测试结果汇总                                   ║"
echo "╚══════════════════════════════════════════════════════════╝"

idx=0
has_fail=false
for entry in "${TEST_FUNCTIONS[@]}"; do
    func="${entry%%:*}"
    name="${entry##*:}"

    if [ -n "$FILTER" ] && [ "${func#test_}" != "$FILTER" ]; then
        continue
    fi

    result="${SUITE_RESULTS[$idx]}"
    if [ "$result" = "PASS" ]; then
        echo -e "  ${GREEN}[PASS]${NC} $name"
    else
        echo -e "  ${RED}[FAIL]${NC} $name"
        has_fail=true
    fi
    idx=$((idx + 1))
done
echo ""

if $has_fail; then
    echo -e "${RED}部分测试套件未通过!${NC}"
    exit 1
else
    echo -e "${GREEN}所有测试套件通过!${NC}"
fi
