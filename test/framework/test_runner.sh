#!/bin/bash
# StrixDB 统一 SQL 测试框架
# 使用方式: source 本文件，然后调用框架函数编写测试
#
# 提供的函数:
#   framework_init <db_name>        — 初始化测试环境，启动 server
#   framework_cleanup               — 清理测试环境，停止 server
#   run_sql <sql>                   — 执行一条 SQL，输出到 stdout
#   send_sql <sql>                  — 执行一条 SQL，不输出（用于 DDL/DML）
#   check_contains <desc> <output> <pattern>       — 检查输出包含某模式
#   check_not_contains <desc> <output> <pattern>   — 检查输出不包含某模式
#   check_count <desc> <output> <pattern> <expected_count>  — 检查模式出现次数
#   framework_summary               — 输出测试结果汇总
#
# 全局变量:
#   PASS, FAIL  — 测试计数
#   FRAMEWORK_SERVER_PID
#   FRAMEWORK_DB_NAME
#   FRAMEWORK_SERVER_LOG

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SERVER="${PROJECT_ROOT}/build/bin/rmdb"
CLIENT="${PROJECT_ROOT}/build/bin/rmdb_client"
HOST="127.0.0.1"
PORT="8765"

PASS=0
FAIL=0
FRAMEWORK_SERVER_PID=""
FRAMEWORK_DB_NAME=""
FRAMEWORK_SERVER_LOG=""

framework_init() {
    FRAMEWORK_DB_NAME="$1"
    FRAMEWORK_SERVER_LOG="/tmp/rmdb_${FRAMEWORK_DB_NAME}_$$.log"

    echo "=== [framework] 清理旧的数据库 ==="
    rm -rf "$FRAMEWORK_DB_NAME"

    echo "=== [framework] 启动 server (后台) ==="
    "$SERVER" "$FRAMEWORK_DB_NAME" > "$FRAMEWORK_SERVER_LOG" 2>&1 &
    FRAMEWORK_SERVER_PID=$!
    sleep 1

    if ! kill -0 "$FRAMEWORK_SERVER_PID" 2>/dev/null; then
        echo "ERROR: Server failed to start"
        cat "$FRAMEWORK_SERVER_LOG"
        exit 1
    fi
    echo "  Server PID=$FRAMEWORK_SERVER_PID"
}

framework_cleanup() {
    echo ""
    echo "=== [framework] 清理 ==="
    if [ -n "$FRAMEWORK_SERVER_PID" ]; then
        kill "$FRAMEWORK_SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "$FRAMEWORK_DB_NAME" ]; then
        rm -rf "$FRAMEWORK_DB_NAME"
    fi
    if [ -n "$FRAMEWORK_SERVER_LOG" ]; then
        rm -f "$FRAMEWORK_SERVER_LOG"
    fi
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
    local desc="$1"
    local output="$2"
    local pattern="$3"
    if echo "$output" | grep -q "$pattern"; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc — expected '$pattern' not found in output"
        FAIL=$((FAIL + 1))
    fi
}

check_not_contains() {
    local desc="$1"
    local output="$2"
    local pattern="$3"
    if echo "$output" | grep -q "$pattern"; then
        echo "  [FAIL] $desc — unexpected '$pattern' found in output"
        FAIL=$((FAIL + 1))
    else
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    fi
}

# 通过 "Total record(s): N" 检查行数（不受表头/分隔线影响）
check_row_count() {
    local desc="$1"
    local output="$2"
    local expected="$3"
    local actual
    actual=$(echo "$output" | grep -oP 'Total record\(s\): \K\d+' || echo "0")
    if [ "$actual" -eq "$expected" ]; then
        echo "  [PASS] $desc (rows=$actual)"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc — expected $expected rows, got $actual"
        FAIL=$((FAIL + 1))
    fi
}

# 检查模式出现次数（从输出中提取纯数据行进行匹配）
# 自动跳过表头和分隔线的干扰
check_count() {
    local desc="$1"
    local output="$2"
    local pattern="$3"
    local expected="$4"
    local actual
    # 只对以 "| " 开头且包含数字的行进行计数（跳过表头）
    actual=$(echo "$output" | grep -E '^\| ' | grep -v -E '^\| *[a-zA-Z]' | grep -c "$pattern" || true)
    if [ "$actual" -eq "$expected" ]; then
        echo "  [PASS] $desc (count=$actual)"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc — expected count=$expected, got $actual"
        FAIL=$((FAIL + 1))
    fi
}

check_not_contains_regex() {
    local desc="$1"
    local output="$2"
    local pattern="$3"
    if echo "$output" | grep -Eq "$pattern"; then
        echo "  [FAIL] $desc — unexpected pattern '$pattern' found"
        FAIL=$((FAIL + 1))
    else
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    fi
}

framework_summary() {
    echo ""
    echo "============================================"
    echo "  测试结果: 通过 $PASS, 失败 $FAIL"
    echo "============================================"
    if [ "$FAIL" -gt 0 ]; then
        return 1
    fi
    return 0
}
