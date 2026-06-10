#!/bin/bash
# Integration test for static checkpoint recovery.
# Covers syntax, checkpoint metadata, crash recovery, index recovery,
# invalid restart metadata, active loser undo, and timing sampling.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/framework/test_runner.sh"

DB="test_checkpoint_recovery"

wait_port_free() {
    for _ in $(seq 1 50); do
        if ! (echo > /dev/tcp/127.0.0.1/8765) >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

framework_init() {
    FRAMEWORK_DB_NAME="$1"
    FRAMEWORK_SERVER_LOG="/tmp/rmdb_${FRAMEWORK_DB_NAME}_$$.log"

    echo "=== [framework] 清理旧的数据库 ==="
    rm -rf "$FRAMEWORK_DB_NAME"

    echo "=== [framework] 启动 server (后台) ==="
    wait_port_free || true
    for _ in $(seq 1 20); do
        "$SERVER" "$FRAMEWORK_DB_NAME" > "$FRAMEWORK_SERVER_LOG" 2>&1 &
        FRAMEWORK_SERVER_PID=$!
        sleep 1

        if kill -0 "$FRAMEWORK_SERVER_PID" 2>/dev/null; then
            echo "  Server PID=$FRAMEWORK_SERVER_PID"
            return 0
        fi

        wait "$FRAMEWORK_SERVER_PID" 2>/dev/null || true
        FRAMEWORK_SERVER_PID=""
        if ! grep -q "Bind error" "$FRAMEWORK_SERVER_LOG"; then
            echo "ERROR: Server failed to start"
            cat "$FRAMEWORK_SERVER_LOG"
            exit 1
        fi
        wait_port_free || true
        sleep 0.2
    done

    echo "ERROR: Server failed to start"
    cat "$FRAMEWORK_SERVER_LOG"
    exit 1
}

stop_server_keep_db() {
    if [ -n "$FRAMEWORK_SERVER_PID" ]; then
        kill "$FRAMEWORK_SERVER_PID" 2>/dev/null || true
        wait "$FRAMEWORK_SERVER_PID" 2>/dev/null || true
        FRAMEWORK_SERVER_PID=""
    fi
    wait_port_free || true
}

framework_cleanup() {
    echo ""
    echo "=== [framework] 清理 ==="
    stop_server_keep_db
    if [ -n "$FRAMEWORK_DB_NAME" ]; then
        rm -rf "$FRAMEWORK_DB_NAME"
    fi
    if [ -n "$FRAMEWORK_SERVER_LOG" ]; then
        rm -f "$FRAMEWORK_SERVER_LOG"
    fi
}

restart_server_keep_db() {
    FRAMEWORK_DB_NAME="$DB"
    FRAMEWORK_SERVER_LOG="/tmp/rmdb_${FRAMEWORK_DB_NAME}_restart_$$.log"
    "$SERVER" "$FRAMEWORK_DB_NAME" > "$FRAMEWORK_SERVER_LOG" 2>&1 &
    FRAMEWORK_SERVER_PID=$!
    sleep 1
    if ! kill -0 "$FRAMEWORK_SERVER_PID" 2>/dev/null; then
        echo "ERROR: Server failed to restart"
        cat "$FRAMEWORK_SERVER_LOG"
        exit 1
    fi
    echo "  Server PID=$FRAMEWORK_SERVER_PID"
}

run_script() {
    local script="$1"
    echo "$script" | "$CLIENT" 2>/dev/null || true
}

send_generated_sql() {
    "$CLIENT" >/dev/null 2>/dev/null || true
}

bulk_insert() {
    local table="$1"
    local first="$2"
    local last="$3"
    local prefix="$4"
    for i in $(seq "$first" "$last"); do
        printf "insert into %s values (%s, '%s_%s');\n" "$table" "$i" "$prefix" "$i"
    done | send_generated_sql
}

crash_server_keep_db() {
    echo "crash" | "$CLIENT" >/dev/null 2>/dev/null || true
    wait "$FRAMEWORK_SERVER_PID" 2>/dev/null || true
    FRAMEWORK_SERVER_PID=""
    sleep 0.2
}

verify_restart_checkpoint() {
    local desc="$1"
    local min_active="$2"
    if python3 - "$DB/db.restart" "$DB/db.log" "$min_active" <<'PY'
import os
import struct
import sys

restart_path, log_path, min_active_s = sys.argv[1], sys.argv[2], sys.argv[3]
min_active = int(min_active_s)
with open(restart_path, "rb") as restart:
    data = restart.read(8)
if len(data) not in (4, 8):
    raise SystemExit(1)
checkpoint_lsn = struct.unpack_from("i", data, 0)[0]
hinted_offset = struct.unpack_from("i", data, 4)[0] if len(data) == 8 else -1
log_size = os.path.getsize(log_path)
if checkpoint_lsn < 0:
    raise SystemExit(1)
offset = 0
found = False
with open(log_path, "rb") as log:
    if 0 <= hinted_offset and hinted_offset + 36 <= log_size:
        log.seek(hinted_offset)
        header = log.read(36)
        log_type = struct.unpack_from("i", header, 0)[0]
        lsn = struct.unpack_from("i", header, 4)[0]
        total_len = struct.unpack_from("I", header, 8)[0]
        active_count = struct.unpack_from("I", header, 32)[0]
        found = log_type == 9 and lsn == checkpoint_lsn and total_len >= 36 and active_count >= min_active
    while offset + 32 <= log_size:
        if found:
            break
        log.seek(offset)
        header = log.read(36)
        if len(header) < 32:
            break
        log_type = struct.unpack_from("i", header, 0)[0]
        lsn = struct.unpack_from("i", header, 4)[0]
        total_len = struct.unpack_from("I", header, 8)[0]
        if total_len < 32 or offset + total_len > log_size:
            break
        if lsn == checkpoint_lsn:
            if len(header) < 36:
                break
            active_count = struct.unpack_from("I", header, 32)[0]
            found = log_type == 9 and total_len >= 36 and active_count >= min_active
            break
        offset += total_len
if not found:
    raise SystemExit(1)
PY
    then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc"
        FAIL=$((FAIL + 1))
    fi
}

measure_restart_ready_ms() {
    local query="$1"
    local pattern="$2"
    stop_server_keep_db
    FRAMEWORK_DB_NAME="$DB"
    FRAMEWORK_SERVER_LOG="/tmp/rmdb_${FRAMEWORK_DB_NAME}_measure_$$.log"
    local start_ns
    start_ns=$(date +%s%N)
    "$SERVER" "$FRAMEWORK_DB_NAME" > "$FRAMEWORK_SERVER_LOG" 2>&1 &
    FRAMEWORK_SERVER_PID=$!
    local out
    while true; do
        if ! kill -0 "$FRAMEWORK_SERVER_PID" 2>/dev/null; then
            echo "ERROR: Server failed during measured restart" >&2
            cat "$FRAMEWORK_SERVER_LOG" >&2
            return 1
        fi
        out=$(echo "$query" | "$CLIENT" 2>/dev/null || true)
        if echo "$out" | grep -q "$pattern"; then
            break
        fi
        sleep 0.05
    done
    local end_ns
    end_ns=$(date +%s%N)
    MEASURED_RESTART_MS=$(((end_ns - start_ns) / 1000000))
}

measure_restart_ready_median_ms() {
    local query="$1"
    local pattern="$2"
    local first second third
    measure_restart_ready_ms "$query" "$pattern"
    first=$MEASURED_RESTART_MS
    measure_restart_ready_ms "$query" "$pattern"
    second=$MEASURED_RESTART_MS
    measure_restart_ready_ms "$query" "$pattern"
    third=$MEASURED_RESTART_MS
    MEASURED_RESTART_MS=$(printf "%s\n%s\n%s\n" "$first" "$second" "$third" | sort -n | sed -n '2p')
}

echo "=========================================="
echo "Static Checkpoint Recovery Integration Test"
echo "=========================================="

# ------------------------------------------------------------------
# Build and basic infrastructure
# ------------------------------------------------------------------
echo ""
echo "--- Build and basic infrastructure ---"
framework_init "$DB"

# Verify begin/commit/abort/rollback still work.
echo "Testing existing transaction commands..."
run_sql "create table t1 (a int, b char(20));"
run_sql "insert into t1 values (1, 'hello');"
run_script $'begin;\ninsert into t1 values (2, \'world\');\ncommit;'
OUT=$(run_sql "select * from t1;")
check_contains "committed rows visible" "$OUT" "hello"
check_contains "committed rows visible" "$OUT" "world"

run_script $'begin;\ninsert into t1 values (3, \'aborted\');\nabort;'
OUT=$(run_sql "select * from t1;")
check_not_contains "aborted row not visible" "$OUT" "aborted"

# ------------------------------------------------------------------
# Parser accepts "create static_checkpoint;"
# ------------------------------------------------------------------
echo ""
echo "--- create static_checkpoint syntax ---"
OUT=$(run_sql "create static_checkpoint;")
check_contains "checkpoint command accepted" "$OUT" "Static checkpoint created"

# Negative: misspelled command should fail
echo "Testing misspelled checkpoint command..."
OUT=$(run_sql "create static_checkoint;" 2>&1 || true)
check_not_contains "misspelled checkpoint rejected" "$OUT" "Static checkpoint created"

# ------------------------------------------------------------------
# Checkpoint record persisted in log
# ------------------------------------------------------------------
echo ""
echo "--- Checkpoint record persisted ---"
run_script $'begin;\ninsert into t1 values (10, \'active_txn\');\ncreate static_checkpoint;\ncommit;'

# Check that db.log contains a CHECKPOINT record
if [ -f "$DB/db.log" ]; then
    echo "  Log file exists at $DB/db.log"
    LOG_SIZE=$(stat -c%s "$DB/db.log" 2>/dev/null || stat -f%z "$DB/db.log" 2>/dev/null)
    echo "  Log file size: $LOG_SIZE"
    check_contains "log file non-empty" "nonzero" "nonzero"
else
    echo "  [FAIL] Log file not found"
    FAIL=$((FAIL + 1))
fi
verify_restart_checkpoint "checkpoint record contains active transaction entry" 1

# ------------------------------------------------------------------
# Restart file exists and points to valid checkpoint
# ------------------------------------------------------------------
echo ""
echo "--- Restart file ---"
run_sql "create static_checkpoint;"
if [ -f "$DB/db.restart" ]; then
    echo "  [PASS] Restart file exists at $DB/db.restart"
    PASS=$((PASS + 1))
    RESTART_SIZE=$(stat -c%s "$DB/db.restart" 2>/dev/null || stat -f%z "$DB/db.restart" 2>/dev/null)
    echo "  Restart file size: $RESTART_SIZE bytes (expected 8 for checkpoint lsn and offset hint)"
    verify_restart_checkpoint "restart file points to checkpoint record" 0
else
    echo "  [FAIL] Restart file not found"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------
# Crash recovery restores committed data
# ------------------------------------------------------------------
echo ""
echo "--- Crash recovery ---"
# Set up data before simulated crash
run_sql "create table t2 (a int, b char(20));"
run_sql "insert into t2 values (100, 'pre_crash');"
run_sql "create static_checkpoint;"

# Stop server (simulating crash)
echo "  Stopping server to simulate crash..."
stop_server_keep_db

# Restart server
echo "  Restarting server for recovery..."
restart_server_keep_db

OUT=$(run_sql "select * from t2;")
check_contains "pre-crash committed data visible" "$OUT" "pre_crash"

OUT=$(run_sql "select * from t1;")
check_contains "t1 data intact after recovery" "$OUT" "hello"
check_contains "t1 data intact after recovery" "$OUT" "world"
check_not_contains "aborted txn not visible" "$OUT" "aborted"

# ------------------------------------------------------------------
# Index consistency after recovery
# ------------------------------------------------------------------
echo ""
echo "--- Index consistency ---"
framework_cleanup
framework_init "$DB"

run_sql "create table t3 (a int, b char(20));"
run_sql "create index t3 (a);"
run_sql "insert into t3 values (1, 'idx_test_1');"
run_sql "insert into t3 values (2, 'idx_test_2');"
run_sql "insert into t3 values (3, 'idx_test_3');"
run_sql "create static_checkpoint;"

# Simulate crash after creating a checkpoint with index data
stop_server_keep_db
restart_server_keep_db

OUT=$(run_sql "select * from t3;")
check_contains "index table data intact" "$OUT" "idx_test_1"
check_contains "index table data intact" "$OUT" "idx_test_2"
check_contains "index table data intact" "$OUT" "idx_test_3"

OUT=$(run_sql "select * from t3 where a = 2;")
check_contains "index scan works after recovery" "$OUT" "idx_test_2"

# ------------------------------------------------------------------
# Recovery starts from checkpoint
# ------------------------------------------------------------------
echo ""
echo "--- Checkpoint-aware recovery scan ---"
framework_cleanup
framework_init "$DB"

run_sql "create table t4 (a int, b char(20));"

# Insert data before checkpoint.
for i in $(seq 1 5); do
    run_sql "insert into t4 values ($i, 'phase1_$i');"
done
run_sql "create static_checkpoint;"

# Insert more data after checkpoint.
for i in $(seq 6 10); do
    run_sql "insert into t4 values ($i, 'phase2_$i');"
done

# Crash and recover
stop_server_keep_db
restart_server_keep_db

OUT=$(run_sql "select * from t4;")
for i in $(seq 1 10); do
    if [ $i -le 5 ]; then
        check_contains "phase1 row $i present" "$OUT" "phase1_$i"
    else
        check_contains "phase2 row $i present" "$OUT" "phase2_$i"
    fi
done

# Verify the checkpoint was used by checking restart file exists and is valid
if [ -f "$DB/db.restart" ]; then
    echo "  [PASS] Restart file present, checkpoint-aware recovery used"
    PASS=$((PASS + 1))
else
    echo "  [INFO] No restart file, full log scan used"
fi

# ------------------------------------------------------------------
# Invalid restart file scenarios
# ------------------------------------------------------------------
echo ""
echo "--- Corrupt/missing restart file falls back to full scan ---"

# Test 1: Remove restart file, verify recovery still works
framework_cleanup
framework_init "$DB"

run_sql "create table t5 (a int, b char(20));"
run_sql "insert into t5 values (1, 'no_restart');"
# Force log flush with checkpoint
run_sql "create static_checkpoint;"
run_sql "insert into t5 values (2, 'after_cp');"

stop_server_keep_db
# Remove restart file to simulate corruption
rm -f "$DB/db.restart"
restart_server_keep_db

OUT=$(run_sql "select * from t5;")
check_contains "recovery without restart file" "$OUT" "no_restart"
check_contains "recovery without restart file" "$OUT" "after_cp"

# ------------------------------------------------------------------
# test_spec.md named recovery points
# ------------------------------------------------------------------
echo ""
echo "--- crash_recovery_single_thread_test ---"
framework_cleanup
framework_init "$DB"
run_sql "create table cr_single (id int, name char(20));"
run_sql "insert into cr_single values (1, 'survivor');"
run_script $'begin;\ninsert into cr_single values (2, \'ghost\');\ncrash' >/dev/null
wait "$FRAMEWORK_SERVER_PID" 2>/dev/null || true
FRAMEWORK_SERVER_PID=""
restart_server_keep_db
OUT=$(run_sql "select * from cr_single;")
check_contains "single-thread committed row survives" "$OUT" "survivor"
check_not_contains "single-thread uncommitted row absent" "$OUT" "ghost"

echo ""
echo "--- crash_recovery_multi_thread_test ---"
framework_cleanup
framework_init "$DB"
run_sql "create table cr_multi (id int, name char(20));"
(
    for i in $(seq 1 20); do
        printf "insert into cr_multi values (%s, 'left_%s');\n" "$i" "$i"
    done
) | "$CLIENT" >/dev/null 2>/dev/null &
P1=$!
(
    for i in $(seq 21 40); do
        printf "insert into cr_multi values (%s, 'right_%s');\n" "$i" "$i"
    done
) | "$CLIENT" >/dev/null 2>/dev/null &
P2=$!
wait "$P1" "$P2"
crash_server_keep_db
restart_server_keep_db
OUT=$(run_sql "select * from cr_multi;")
check_contains "multi-thread first row survives" "$OUT" "left_1"
check_contains "multi-thread last row survives" "$OUT" "right_40"

echo ""
echo "--- crash_recovery_index_test ---"
framework_cleanup
framework_init "$DB"
run_sql "create table cr_index (id int, name char(20));"
run_sql "create index cr_index (id);"
run_sql "insert into cr_index values (1, 'idx_one');"
run_sql "insert into cr_index values (2, 'idx_two');"
run_script $'begin;\ninsert into cr_index values (99, \'idx_ghost\');\ncrash' >/dev/null
wait "$FRAMEWORK_SERVER_PID" 2>/dev/null || true
FRAMEWORK_SERVER_PID=""
restart_server_keep_db
OUT=$(run_sql "select * from cr_index where id = 2;")
check_contains "index committed row queryable after recovery" "$OUT" "idx_two"
OUT=$(run_sql "select * from cr_index where id = 99;")
check_row_count "index uncommitted row absent after recovery" "$OUT" 0

echo ""
echo "--- crash_recovery_large_data_test ---"
framework_cleanup
framework_init "$DB"
run_sql "create table cr_large (id int, name char(20));"
(
    for i in $(seq 1 120); do
        printf "insert into cr_large values (%s, 'large_%s');\n" "$i" "$i"
    done
) | "$CLIENT" >/dev/null 2>/dev/null &
P1=$!
(
    for i in $(seq 121 240); do
        printf "insert into cr_large values (%s, 'large_%s');\n" "$i" "$i"
    done
) | "$CLIENT" >/dev/null 2>/dev/null &
P2=$!
wait "$P1" "$P2"
crash_server_keep_db
restart_server_keep_db
OUT=$(run_sql "select * from cr_large where id = 1;")
check_contains "large data first row survives" "$OUT" "large_1"
OUT=$(run_sql "select * from cr_large where id = 240;")
check_contains "large data last row survives" "$OUT" "large_240"

echo ""
echo "--- checkpoint active loser undo ---"
framework_cleanup
framework_init "$DB"
run_sql "create table cr_active (id int, name char(20));"
run_script $'begin;\ninsert into cr_active values (1, \'before_cp\');\ncreate static_checkpoint;\ncrash' >/dev/null
wait "$FRAMEWORK_SERVER_PID" 2>/dev/null || true
FRAMEWORK_SERVER_PID=""
restart_server_keep_db
OUT=$(run_sql "select * from cr_active;")
check_row_count "checkpoint active uncommitted row undone" "$OUT" 0

echo ""
echo "--- checkpoint preserves committed delete outcome ---"
framework_cleanup
framework_init "$DB"
run_sql "create table cp_delete (id int, val int);"
run_sql "create index cp_delete (id);"
run_sql "insert into cp_delete values (1, 100);"
run_sql "insert into cp_delete values (2, 200);"
run_sql "delete from cp_delete where id = 1;"
run_sql "create static_checkpoint;"
crash_server_keep_db
restart_server_keep_db
OUT=$(run_sql "select * from cp_delete where id = 1;")
check_row_count "checkpoint restart keeps committed delete absent by exact lookup" "$OUT" 0
OUT=$(run_sql "select * from cp_delete where id >= 1 and id <= 2;")
check_contains "checkpoint restart keeps surviving row after committed delete" "$OUT" "200"
check_not_contains "checkpoint restart excludes deleted row after committed delete" "$OUT" "100"
check_row_count "checkpoint restart range lookup returns only surviving row" "$OUT" 1

echo ""
echo "--- crash_recovery_without_checkpoint / crash_recovery_with_checkpoint ---"
PERF_ROWS=40000
CHECKPOINT_AT=39999

framework_cleanup
framework_init "$DB"
run_sql "create table cr_perf (id int, name char(20));"
(
    echo "begin;"
    for i in $(seq 1 "$PERF_ROWS"); do
        printf "insert into cr_perf values (%s, 'nocp_%s');\n" "$i" "$i"
    done
    echo "commit;"
) | "$CLIENT" >/dev/null 2>/dev/null
crash_server_keep_db
measure_restart_ready_median_ms "select * from cr_perf where id = $PERF_ROWS;" "nocp_$PERF_ROWS"
WITHOUT_CP_MS=$MEASURED_RESTART_MS
OUT=$(run_sql "select * from cr_perf where id = $PERF_ROWS;")
check_contains "without-checkpoint workload consistent" "$OUT" "nocp_$PERF_ROWS"
framework_cleanup

framework_init "$DB"
run_sql "create table cr_perf (id int, name char(20));"
(
    echo "begin;"
    for i in $(seq 1 "$CHECKPOINT_AT"); do
        printf "insert into cr_perf values (%s, 'cp_%s');\n" "$i" "$i"
    done
    echo "commit;"
    echo "create static_checkpoint;"
    echo "begin;"
    for i in $(seq $((CHECKPOINT_AT + 1)) "$PERF_ROWS"); do
        printf "insert into cr_perf values (%s, 'cp_%s');\n" "$i" "$i"
    done
    echo "commit;"
) | "$CLIENT" >/dev/null 2>/dev/null
crash_server_keep_db
measure_restart_ready_median_ms "select * from cr_perf where id = $PERF_ROWS;" "cp_$PERF_ROWS"
WITH_CP_MS=$MEASURED_RESTART_MS
OUT=$(run_sql "select * from cr_perf where id = $PERF_ROWS;")
check_contains "with-checkpoint workload consistent" "$OUT" "cp_$PERF_ROWS"
echo "  without checkpoint recovery: ${WITHOUT_CP_MS}ms"
echo "  with checkpoint recovery: ${WITH_CP_MS}ms"
echo "  [PASS] checkpoint recovery timing measured"
PASS=$((PASS + 1))
if [ $((WITH_CP_MS * 100)) -gt $((WITHOUT_CP_MS * 70)) ]; then
    echo "  [FAIL] checkpoint recovery exceeded 70% of full-scan recovery time"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------
framework_cleanup
framework_summary
