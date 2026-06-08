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
    t2.sql("update deadlock_t set val = 21 where id = 1;")
    t1.sql("update deadlock_t set val = 12 where id = 2;")
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
    t2.sql("update update_t set val = 30 where id = 1;")
    t1.sql("commit;")
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


CASES = [
    ("si_Deadlock", case_deadlock, EXPECTED_DEADLOCK),
    ("si_Non_Repeatable_Read_Lost_Update", case_non_repeatable_read_lost_update,
     EXPECTED_NON_REPEATABLE_READ_LOST_UPDATE),
    ("si_UpdateTest", case_update_test, EXPECTED_UPDATE_TEST),
    ("si_WriteWriteConflictDeleteInsertTest", case_delete_insert_conflict, EXPECTED_DELETE_INSERT_CONFLICT),
    ("ser_phantom_read_test_4", case_phantom_read_test_4, EXPECTED_PHANTOM_READ_TEST_4),
]


failed = 0
for case_name, action, expected_output in CASES:
    if not run_case(case_name, action, expected_output):
        failed += 1

sys.exit(1 if failed else 0)
PY

PROJECT_ROOT="${PROJECT_ROOT}" python3 "${CHECKER}"
rm -f "${CHECKER}"
