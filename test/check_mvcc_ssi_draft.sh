#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHECKER="/tmp/mvcc_ssi_draft_check_$$.py"

cat > "${CHECKER}" <<'PY'
import difflib
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.environ["PROJECT_ROOT"]
SERVER = os.path.join(ROOT, "build/bin/rmdb")

SEP = "+------------------+------------------+\n"


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


class AsyncSql:
    def __init__(self, client, query):
        self.client = client
        self.query = query
        self.result = None
        self.error = None
        import threading
        self.thread = threading.Thread(target=self._run)
        self.thread.start()

    def _run(self):
        try:
            self.result = self.client.sql(self.query)
        except BaseException as exc:
            self.error = exc

    def wait(self, timeout=5):
        self.thread.join(timeout)
        if self.thread.is_alive():
            raise TimeoutError(f"timed out waiting for {self.query}")
        if self.error is not None:
            raise self.error
        return self.result


def wait_server():
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            conn = socket.create_connection(("127.0.0.1", 8765), timeout=0.2)
            conn.close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("server did not start")


def wait_port_free():
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            conn = socket.create_connection(("127.0.0.1", 8765), timeout=0.2)
            conn.close()
            time.sleep(0.1)
        except OSError:
            return
    raise RuntimeError("server port did not become free")


def run_case(name, actions, expected):
    db = f"mvcc_ssi_draft_{name}_{os.getpid()}"
    dbpath = os.path.join(ROOT, db)
    log = os.path.join("/tmp", db + ".server.log")

    shutil.rmtree(dbpath, ignore_errors=True)
    wait_port_free()
    log_file = open(log, "w")
    proc = subprocess.Popen([SERVER, db], cwd=ROOT, stdout=log_file, stderr=subprocess.STDOUT)
    clients = {}

    def client(label):
        item = clients.get(label)
        if item is None:
            item = Client()
            clients[label] = item
        return item

    try:
        wait_server()
        actions(client)
    finally:
        for item in list(clients.values()):
            try:
                item.close()
            except OSError:
                pass
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        log_file.close()

    output = os.path.join(dbpath, "output.txt")
    if not os.path.exists(output):
        actual = ""
    else:
        with open(output, encoding="utf-8") as file:
            actual = file.read()

    ok = actual == expected
    if ok:
        print(f"[PASS] {name}")
    else:
        print(f"[FAIL] {name}", file=sys.stderr)
        diff = difflib.unified_diff(
            expected.splitlines(keepends=True),
            actual.splitlines(keepends=True),
            fromfile=f"{name}.expected",
            tofile=f"{name}.output.txt",
        )
        sys.stderr.writelines(diff)

    shutil.rmtree(dbpath, ignore_errors=True)
    try:
        os.remove(log)
    except FileNotFoundError:
        pass
    wait_port_free()
    return ok


def set_level(level, *clients):
    for item in clients:
        item.sql(f"set transaction isolation level {level};")


def async_sql(client, query):
    return AsyncSql(client, query)


def case_deadlock(client):
    setup = client("setup")
    setup.sql("create table deadlock_t (id int, val int);")
    setup.sql("insert into deadlock_t values (1, 10);")
    setup.sql("insert into deadlock_t values (2, 20);")

    t1 = client("t1")
    t2 = client("t2")
    observer = client("observer")
    set_level("snapshot isolation", t1, t2, observer)

    t1.sql("begin;")
    t1.sql("update deadlock_t set val = 11 where id = 1;")
    t2.sql("begin;")
    t2.sql("update deadlock_t set val = 22 where id = 2;")
    t1_wait = async_sql(t1, "update deadlock_t set val = 12 where id = 2;")
    t2.sql("update deadlock_t set val = 21 where id = 1;")
    t1_wait.wait()
    t1.sql("commit;")
    observer.sql("select * from deadlock_t where id >= 1 and id <= 2;")


def case_non_repeatable_read_lost_update(client):
    setup = client("setup")
    setup.sql("create table lost_update_t (id int, val int);")
    setup.sql("insert into lost_update_t values (1, 10);")

    t1 = client("t1")
    t2 = client("t2")
    observer = client("observer")
    set_level("snapshot isolation", t1, t2, observer)

    t1.sql("begin;")
    t1.sql("select * from lost_update_t where id = 1;")
    t2.sql("begin;")
    t2.sql("update lost_update_t set val = 20 where id = 1;")
    t2.sql("commit;")
    t1.sql("select * from lost_update_t where id = 1;")
    t1.sql("update lost_update_t set val = 15 where id = 1;")
    observer.sql("select * from lost_update_t where id = 1;")


def case_update_test(client):
    setup = client("setup")
    setup.sql("create table update_t (id int, val int);")
    setup.sql("insert into update_t values (1, 10);")

    t1 = client("t1")
    t2 = client("t2")
    observer = client("observer")
    set_level("snapshot isolation", t1, t2, observer)

    t1.sql("begin;")
    t1.sql("update update_t set val = 20 where id = 1;")
    t1.sql("select * from update_t where id = 1;")
    t2.sql("begin;")
    t2_wait = async_sql(t2, "update update_t set val = 30 where id = 1;")
    t1.sql("commit;")
    t2_wait.wait()
    observer.sql("select * from update_t where id = 1;")


def case_delete_insert_conflict(client):
    setup = client("setup")
    setup.sql("create table delete_insert_t (id int, val int);")
    setup.sql("insert into delete_insert_t values (1, 10);")
    setup.sql("create index delete_insert_t(id);")

    t1 = client("t1")
    t2 = client("t2")
    t3 = client("t3")
    observer = client("observer")
    set_level("snapshot isolation", t1, t2, t3, observer)

    t1.sql("begin;")
    t1.sql("delete from delete_insert_t where id = 1;")
    t2.sql("begin;")
    t2.sql("insert into delete_insert_t values (1, 20);")
    t1.sql("commit;")
    observer.sql("select * from delete_insert_t where id = 1;")
    t3.sql("begin;")
    t3.sql("insert into delete_insert_t values (1, 30);")
    t3.sql("commit;")
    observer.sql("select * from delete_insert_t where id = 1;")


def case_old_snapshot_index_after_delete(client):
    setup = client("setup")
    setup.sql("create table old_snapshot_index_t (id int, val int);")
    setup.sql("insert into old_snapshot_index_t values (1, 10);")
    setup.sql("create index old_snapshot_index_t(id);")

    t1 = client("t1")
    t2 = client("t2")
    set_level("snapshot isolation", t1, t2)

    t1.sql("begin;")
    t1.sql("select * from old_snapshot_index_t where id = 1;")
    t2.sql("begin;")
    t2.sql("delete from old_snapshot_index_t where id = 1;")
    t2.sql("commit;")
    t1.sql("select * from old_snapshot_index_t where id = 1;")
    t1.sql("commit;")


def case_phantom_read_test_4(client):
    setup = client("setup")
    setup.sql("create table phantom_t (id int, val int);")
    setup.sql("insert into phantom_t values (1, 10);")
    setup.sql("insert into phantom_t values (30, 30);")

    t1 = client("t1")
    t2 = client("t2")
    t3 = client("t3")
    set_level("serializable", t1, t2, t3)

    t1.sql("begin;")
    t1.sql("select * from phantom_t where id >= 10 and id <= 20;")
    t2.sql("begin;")
    t2.sql("select * from phantom_t where id = 1;")
    t1.sql("update phantom_t set val = 11 where id = 1;")
    t2.sql("insert into phantom_t values (10, 100);")
    t1.sql("commit;")
    t3.sql("select * from phantom_t where id = 10;")
    t3.sql("select * from phantom_t where id = 1;")


def case_ser_uncommitted_tout_not_abort(client):
    setup = client("setup")
    setup.sql("create table ssi_tout_t (id int, val int);")
    setup.sql("insert into ssi_tout_t values (1, 10);")
    setup.sql("insert into ssi_tout_t values (2, 20);")

    tin = client("tin")
    pivot = client("pivot")
    tout = client("tout")
    observer = client("observer")
    set_level("serializable", tin, pivot, tout, observer)

    pivot.sql("begin;")
    pivot.sql("select * from ssi_tout_t where id = 2;")
    tout.sql("begin;")
    tout.sql("update ssi_tout_t set val = 21 where id = 2;")
    tin.sql("begin;")
    tin.sql("select * from ssi_tout_t where id = 1;")
    pivot.sql("update ssi_tout_t set val = 11 where id = 1;")
    pivot.sql("commit;")
    tout.sql("rollback;")
    observer.sql("select * from ssi_tout_t where id = 1;")


def case_same_txn_delete_insert_unique_key(client):
    setup = client("setup")
    setup.sql("create table same_txn_reinsert_t (id int, val int);")
    setup.sql("insert into same_txn_reinsert_t values (1, 10);")
    setup.sql("create index same_txn_reinsert_t(id);")

    t1 = client("t1")
    observer = client("observer")
    set_level("snapshot isolation", t1, observer)

    t1.sql("begin;")
    t1.sql("delete from same_txn_reinsert_t where id = 1;")
    t1.sql("insert into same_txn_reinsert_t values (1, 20);")
    t1.sql("commit;")
    observer.sql("select * from same_txn_reinsert_t where id = 1;")


def case_concurrent_insert_same_key(client):
    setup = client("setup")
    setup.sql("create table concurrent_insert_t (id int, val int);")
    setup.sql("create index concurrent_insert_t(id);")

    t1 = client("t1")
    t2 = client("t2")
    observer = client("observer")
    set_level("snapshot isolation", t1, t2, observer)

    t1.sql("begin;")
    t1.sql("insert into concurrent_insert_t values (1, 10);")
    t2.sql("begin;")
    t2.sql("insert into concurrent_insert_t values (1, 20);")
    t1.sql("commit;")
    observer.sql("select * from concurrent_insert_t where id = 1;")


def case_snapshot_insert_after_concurrent_commit(client):
    setup = client("setup")
    setup.sql("create table stale_insert_t (id int, val int);")
    setup.sql("create index stale_insert_t(id);")

    t1 = client("t1")
    t2 = client("t2")
    observer = client("observer")
    set_level("snapshot isolation", t1, t2, observer)

    t2.sql("begin;")
    t1.sql("begin;")
    t1.sql("insert into stale_insert_t values (1, 10);")
    t1.sql("commit;")
    t2.sql("insert into stale_insert_t values (1, 20);")
    observer.sql("select * from stale_insert_t where id = 1;")


def case_concurrent_insert_different_keys(client):
    setup = client("setup")
    setup.sql("create table concurrent_insert_distinct_t (id int, val int);")
    setup.sql("create index concurrent_insert_distinct_t(id);")

    t1 = client("t1")
    t2 = client("t2")
    observer = client("observer")
    set_level("snapshot isolation", t1, t2, observer)

    t1.sql("begin;")
    t1.sql("insert into concurrent_insert_distinct_t values (1, 10);")
    t2.sql("begin;")
    t2.sql("insert into concurrent_insert_distinct_t values (2, 20);")
    t1.sql("commit;")
    t2.sql("commit;")
    observer.sql("select * from concurrent_insert_distinct_t where id >= 1 and id <= 2;")


def case_delete_delete_snapshot_conflict(client):
    setup = client("setup")
    setup.sql("create table delete_delete_t (id int, val int);")
    setup.sql("insert into delete_delete_t values (1, 10);")

    t1 = client("t1")
    t2 = client("t2")
    observer = client("observer")
    set_level("snapshot isolation", t1, t2, observer)

    t2.sql("begin;")
    t1.sql("begin;")
    t1.sql("delete from delete_delete_t where id = 1;")
    t1.sql("commit;")
    t2.sql("delete from delete_delete_t where id = 1;")
    observer.sql("select * from delete_delete_t where id = 1;")


def case_unique_violation_cleans_heap_insert(client):
    setup = client("setup")
    setup.sql("create table unique_cleanup_t (id int, val int);")
    setup.sql("insert into unique_cleanup_t values (1, 10);")
    setup.sql("create index unique_cleanup_t(id);")

    session = client("session")
    observer = client("observer")
    set_level("snapshot isolation", session, observer)

    session.sql("insert into unique_cleanup_t values (1, 99);")
    session.sql("insert into unique_cleanup_t values (2, 20);")
    observer.sql("select * from unique_cleanup_t where id >= 1 and id <= 2;")


EXPECTED_DEADLOCK = """abort
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               11 |
|                2 |               12 |
+------------------+------------------+
Total record(s): 2
"""

EXPECTED_NON_REPEATABLE_READ_LOST_UPDATE = """+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
+------------------+------------------+
Total record(s): 1
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
+------------------+------------------+
Total record(s): 1
abort
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               20 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_UPDATE_TEST = """+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               20 |
+------------------+------------------+
Total record(s): 1
abort
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               20 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_DELETE_INSERT_CONFLICT = """abort
+------------------+------------------+
|               id |              val |
+------------------+------------------+
+------------------+------------------+
Total record(s): 0
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               30 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_OLD_SNAPSHOT_INDEX_AFTER_DELETE = """+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
+------------------+------------------+
Total record(s): 1
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_PHANTOM_READ_TEST_4 = """+------------------+------------------+
|               id |              val |
+------------------+------------------+
+------------------+------------------+
Total record(s): 0
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
+------------------+------------------+
Total record(s): 1
abort
+------------------+------------------+
|               id |              val |
+------------------+------------------+
+------------------+------------------+
Total record(s): 0
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               11 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_SER_UNCOMMITTED_TOUT_NOT_ABORT = """+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                2 |               20 |
+------------------+------------------+
Total record(s): 1
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
+------------------+------------------+
Total record(s): 1
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               11 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_SAME_TXN_DELETE_INSERT_UNIQUE_KEY = """+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               20 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_CONCURRENT_INSERT_SAME_KEY = """abort
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_SNAPSHOT_INSERT_AFTER_CONCURRENT_COMMIT = """abort
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
+------------------+------------------+
Total record(s): 1
"""

EXPECTED_CONCURRENT_INSERT_DIFFERENT_KEYS = """+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
|                2 |               20 |
+------------------+------------------+
Total record(s): 2
"""

EXPECTED_DELETE_DELETE_SNAPSHOT_CONFLICT = """abort
+------------------+------------------+
|               id |              val |
+------------------+------------------+
+------------------+------------------+
Total record(s): 0
"""

EXPECTED_UNIQUE_VIOLATION_CLEANS_HEAP_INSERT = """failure
+------------------+------------------+
|               id |              val |
+------------------+------------------+
|                1 |               10 |
|                2 |               20 |
+------------------+------------------+
Total record(s): 2
"""


CASES = [
    ("si_Deadlock", case_deadlock, EXPECTED_DEADLOCK),
    ("si_Non_Repeatable_Read_Lost_Update", case_non_repeatable_read_lost_update,
     EXPECTED_NON_REPEATABLE_READ_LOST_UPDATE),
    ("si_UpdateTest", case_update_test, EXPECTED_UPDATE_TEST),
    ("si_WriteWriteConflictDeleteInsertTest", case_delete_insert_conflict, EXPECTED_DELETE_INSERT_CONFLICT),
    ("si_old_snapshot_index_after_delete", case_old_snapshot_index_after_delete,
     EXPECTED_OLD_SNAPSHOT_INDEX_AFTER_DELETE),
    ("ser_phantom_read_test_4", case_phantom_read_test_4, EXPECTED_PHANTOM_READ_TEST_4),
    ("ser_uncommitted_tout_not_abort", case_ser_uncommitted_tout_not_abort,
     EXPECTED_SER_UNCOMMITTED_TOUT_NOT_ABORT),
    ("si_same_txn_delete_insert_unique_key", case_same_txn_delete_insert_unique_key,
     EXPECTED_SAME_TXN_DELETE_INSERT_UNIQUE_KEY),
    ("si_concurrent_insert_same_key", case_concurrent_insert_same_key, EXPECTED_CONCURRENT_INSERT_SAME_KEY),
    ("si_snapshot_insert_after_concurrent_commit", case_snapshot_insert_after_concurrent_commit,
     EXPECTED_SNAPSHOT_INSERT_AFTER_CONCURRENT_COMMIT),
    ("si_concurrent_insert_different_keys", case_concurrent_insert_different_keys,
     EXPECTED_CONCURRENT_INSERT_DIFFERENT_KEYS),
    ("si_delete_delete_snapshot_conflict", case_delete_delete_snapshot_conflict,
     EXPECTED_DELETE_DELETE_SNAPSHOT_CONFLICT),
    ("si_unique_violation_cleans_heap_insert", case_unique_violation_cleans_heap_insert,
     EXPECTED_UNIQUE_VIOLATION_CLEANS_HEAP_INSERT),
]


failed = 0
for case_name, action, expected_output in CASES:
    if not run_case(case_name, action, expected_output):
        failed += 1

sys.exit(1 if failed else 0)
PY

PROJECT_ROOT="${PROJECT_ROOT}" python3 "${CHECKER}"
rm -f "${CHECKER}"
