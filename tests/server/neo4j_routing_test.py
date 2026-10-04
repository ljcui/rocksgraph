"""Exercise Bolt routing and transactions with the official driver and Raft."""

import argparse
import logging
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

from neo4j import GraphDatabase, READ_ACCESS
from neo4j.exceptions import (
    DatabaseUnavailable,
    DriverError,
    Neo4jError,
    NotALeader,
    WriteServiceUnavailable,
)


def pack(value):
    if value is None:
        return b"\xc0"
    if isinstance(value, str):
        data = value.encode()
        header = bytes([0x80 + len(data)]) if len(data) < 16 else b"\xd0" + bytes([len(data)])
        return header + data
    if isinstance(value, int):
        return b"\xcb" + struct.pack(">q", value)
    if isinstance(value, list):
        return bytes([0x90 + len(value)]) + b"".join(pack(item) for item in value)
    if isinstance(value, dict):
        return bytes([0xA0 + len(value)]) + b"".join(
            pack(key) + pack(item) for key, item in value.items()
        )
    raise TypeError(value)


class RawBolt:
    """Small wire client for malformed requests and exact routing metadata."""

    def __init__(self, port, minor=4, routing=False):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=4)
        self.socket.sendall(struct.pack(">IIIII", 0x6060B017, minor << 8 | 4, 0, 0, 0))
        if self.read(4) != struct.pack(">I", minor << 8 | 4):
            raise AssertionError("Bolt version negotiation failed")
        hello = {"principal": "neo4j", "credentials": "password", "scheme": "basic"}
        if routing:
            hello["routing"] = {"address": f"127.0.0.1:{port}"}
        self.send(0x01, hello)
        if self.receive()[0] != 0x70:
            raise AssertionError("Bolt HELLO failed")

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.socket.close()

    def read(self, size):
        result = bytearray()
        while len(result) < size:
            data = self.socket.recv(size - len(result))
            if not data:
                raise EOFError("Bolt connection closed")
            result.extend(data)
        return bytes(result)

    def send(self, tag, *fields):
        body = bytes([0xB0 + len(fields), tag]) + b"".join(pack(field) for field in fields)
        self.socket.sendall(struct.pack(">H", len(body)) + body + b"\x00\x00")

    def receive(self):
        chunks = bytearray()
        while True:
            size = struct.unpack(">H", self.read(2))[0]
            if not size:
                if chunks:
                    break
                continue
            chunks.extend(self.read(size))
        data = memoryview(chunks)
        offset = 0

        def take(size):
            nonlocal offset
            result = bytes(data[offset:offset + size])
            offset += size
            return result

        def unpack():
            marker = take(1)[0]
            if marker < 0x80:
                return marker
            if marker >= 0xF0:
                return marker - 256
            if marker == 0xC0:
                return None
            if marker in (0xC2, 0xC3):
                return marker == 0xC3
            if marker in (0xC8, 0xC9, 0xCA, 0xCB):
                fmt = {0xC8: ">b", 0xC9: ">h", 0xCA: ">i", 0xCB: ">q"}[marker]
                return struct.unpack(fmt, take(struct.calcsize(fmt)))[0]
            if marker & 0xF0 == 0x80:
                return take(marker & 15).decode()
            if marker in (0xD0, 0xD1, 0xD2):
                fmt = {0xD0: ">B", 0xD1: ">H", 0xD2: ">I"}[marker]
                return take(struct.unpack(fmt, take(struct.calcsize(fmt)))[0]).decode()
            if marker & 0xF0 == 0x90:
                return [unpack() for _ in range(marker & 15)]
            if marker & 0xF0 == 0xA0:
                return {unpack(): unpack() for _ in range(marker & 15)}
            raise AssertionError(f"Unexpected PackStream marker: {marker:#x}")

        header, tag = take(2)
        fields = [unpack() for _ in range(header & 15)]
        if offset != len(data):
            raise AssertionError("Trailing Bolt response data")
        return tag, fields

    def route(self, graph=None, minor=4):
        self.send(0x66, {}, [], ({"db": graph} if graph else {}) if minor >= 4 else graph)
        tag, fields = self.receive()
        if tag != 0x70:
            raise AssertionError(fields)
        return fields[0]["rt"]

    def success(self, tag, *fields):
        self.send(tag, *fields)
        response_tag, response = self.receive()
        if response_tag != 0x70:
            raise AssertionError((response_tag, response))
        return response[0]

    def run(self, query, parameters=None, extra=None):
        return self.success(0x10, query, parameters or {}, extra or {})

    def stream(self, n=-1, qid=-1, discard=False):
        extra = {"n": n, "qid": qid}
        self.send(0x2F if discard else 0x3F, extra)
        rows = []
        while True:
            tag, fields = self.receive()
            if tag == 0x71:
                rows.append(fields[0])
            elif tag == 0x70:
                return rows, fields[0]
            else:
                raise AssertionError((tag, fields))


class Neo4jRoutingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="rocksgraph_neo4j_routing_")
        cls.root = Path(cls.temp.name)
        cls.processes = []
        cls.drivers = []
        cls.logs = []
        cls.ports = set()
        try:
            cls.members = []
            for index in range(3):
                member = {"node_id": index + 1, "ip": "127.0.0.1",
                          "bolt_port": cls.port(), "raft_port": cls.port()}
                cls.members.append(member)
                node_root = cls.root / str(index + 1)
                node_root.mkdir()
                log = (node_root / "server.out").open("w")
                cls.logs.append(log)
                process = subprocess.Popen([
                    SERVER, f"--data_path={node_root / 'data'}",
                    f"--log_path={node_root / 'log'}", f"--query_log_path={node_root / 'log'}",
                    "--log_level=error", "--host=127.0.0.1",
                    f"--bolt_port={member['bolt_port']}", f"--raft_port={member['raft_port']}",
                    f"--http_port={cls.port()}", f"--raft_node_id={member['node_id']}",
                    "--bolt_worker_thread_num=2", "--bolt_io_thread_num=1",
                    "--raft_scheduler_shards=1", "--assistant_thread_num=1",
                    "--graph_block_cache=16777216", "--raft_log_block_cache=16777216",
                ], cwd=node_root, stdout=log, stderr=subprocess.STDOUT)
                cls.processes.append(process)
                driver = cls.driver(member["bolt_port"], scheme="bolt")
                cls.drivers.append(driver)
                deadline = time.monotonic() + 10
                while True:
                    try:
                        driver.verify_connectivity()
                        break
                    except DriverError:
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise
                        time.sleep(0.1)
            for graph in ("routing_a", "routing_b"):
                members = [dict(member, graph=graph) for member in cls.members]
                for driver in cls.drivers:
                    cls.run_query(driver, "CALL dbms.graph.createGraphWithRaft($name, $members)",
                                  database="system", name=graph, members=members)
                cls.leader(graph)
            # Different graphs must route to different leaders on the same servers.
            first_leader = cls.leader("routing_a")
            target = first_leader % 3 + 1
            if cls.leader("routing_b") != target:
                cls.transfer("routing_b", target)
        except BaseException:
            cls.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.cleanup()

    @classmethod
    def cleanup(cls):
        for driver in cls.drivers:
            driver.close()
        for process in cls.processes:
            if process.poll() is None:
                process.terminate()
        for process in cls.processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        for log in cls.logs:
            log.close()
        cls.temp.cleanup()

    @classmethod
    def port(cls):
        while True:
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            if port not in cls.ports:
                cls.ports.add(port)
                return port

    @staticmethod
    def driver(port, scheme="neo4j", host="127.0.0.1", **kwargs):
        options = {"connection_timeout": 1, "connection_acquisition_timeout": 4,
                   "max_transaction_retry_time": 0}
        options.update(kwargs)
        return GraphDatabase.driver(
            f"{scheme}://{host}:{port}", auth=("neo4j", "password"),
            **options,
        )

    @staticmethod
    def run_query(driver, query, database="routing_a", read=False, **parameters):
        options = {"database": database}
        if read:
            options["default_access_mode"] = READ_ACCESS
        with driver.session(**options) as session:
            result = session.run(query, parameters)
            rows = result.data()
            result.consume()
            return rows

    @classmethod
    def leader(cls, graph, expected=None):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            leaders = []
            for index, driver in enumerate(cls.drivers):
                if cls.processes[index].poll() is not None:
                    continue
                rows = cls.run_query(
                    driver, "CALL dbms.graph.getRaftNodeInfos($name) "
                    "YIELD node_id, is_leader RETURN node_id, is_leader",
                    database="system", name=graph,
                )
                leaders.append([row["node_id"] for row in rows if row["is_leader"]])
            if leaders and all(row == leaders[0] and len(row) == 1 for row in leaders):
                candidate = leaders[0][0]
                if cls.processes[candidate - 1].poll() is None and (
                    expected is None or candidate == expected
                ):
                    return candidate
            time.sleep(0.05)
        raise AssertionError(f"No agreed leader for {graph}: {leaders}")

    @classmethod
    def transfer(cls, graph, target):
        current = cls.leader(graph)
        cls.run_query(cls.drivers[current - 1],
                      "CALL dbms.graph.transferRaftLeader($name, $target)",
                      database="system", name=graph, target=target)
        return cls.leader(graph, expected=target)

    def test_01_route_metadata_and_protocol_versions(self):
        for minor in (3, 4):
            with self.subTest(minor=minor), RawBolt(self.members[0]["bolt_port"], minor) as bolt:
                table = bolt.route("routing_a", minor)
                self.assertGreater(table["ttl"], 0)
                self.assertLessEqual(table["ttl"], 10)
                if minor == 4:
                    self.assertEqual(table["db"], "routing_a")
                else:
                    self.assertNotIn("db", table)
                roles = {server["role"]: server["addresses"] for server in table["servers"]}
                self.assertEqual(set(roles), {"ROUTE", "READ", "WRITE"})
                self.assertEqual(set(roles["ROUTE"]),
                                 {f"127.0.0.1:{node['bolt_port']}" for node in self.members})
                leader = self.members[self.leader("routing_a") - 1]
                self.assertEqual(roles["WRITE"], [f"127.0.0.1:{leader['bolt_port']}"])
                self.assertEqual(roles["READ"], roles["WRITE"])

    def test_02_local_and_default_database_routing(self):
        for member in self.members:
            with RawBolt(member["bolt_port"]) as bolt:
                for graph in (None, "system"):
                    table = bolt.route(graph)
                    self.assertEqual(table["db"], graph or "default")
                    for server in table["servers"]:
                        self.assertEqual(server["addresses"], [f"127.0.0.1:{member['bolt_port']}"])
            with self.driver(member["bolt_port"]) as driver:
                driver.verify_connectivity()
                self.assertEqual(self.run_query(driver, "RETURN 1 AS n", database="default"), [{"n": 1}])

    def test_03_invalid_routes_and_reset(self):
        cases = [
            ((), "InputError"),
            ((None, [], {}), "InputError"),
            (({"address": 1}, [], {}), "InputError"),
            (({}, None, {}), "InputError"),
            (({}, [1], {}), "InputError"),
            (({}, ["bookmark"], {}), "Unimplemented"),
            (({}, [], None), "InputError"),
            (({}, [], {"db": 1}), "InputError"),
            (({}, [], {"imp_user": "other"}), "Unimplemented"),
            (({}, [], {"db": "missing_graph"}), "Neo.ClientError.Database.DatabaseNotFound"),
        ]
        with RawBolt(self.members[0]["bolt_port"]) as bolt:
            for fields, code in cases:
                with self.subTest(fields=fields):
                    bolt.send(0x66, *fields)
                    tag, response = bolt.receive()
                    self.assertEqual(tag, 0x7F)
                    self.assertEqual(response[0]["code"], code)
                    bolt.send(0x66, {}, [], {})
                    self.assertEqual(bolt.receive()[0], 0x7E)
                    bolt.send(0x0F)
                    self.assertEqual(bolt.receive()[0], 0x70)
                    self.assertEqual(bolt.route()["db"], "default")
        with RawBolt(self.members[0]["bolt_port"], minor=2) as bolt:
            bolt.send(0x66, {}, [], "default")
            self.assertEqual(bolt.receive()[0], 0x7F)

    def test_04_no_leader_has_no_reader_or_writer(self):
        graph = "routing_no_quorum"
        members = [dict(self.members[0], graph=graph)]
        for node_id in (2, 3):
            members.append({"node_id": node_id, "ip": "::1" if node_id == 3 else "127.0.0.1",
                            "bolt_port": self.port(), "raft_port": self.port(), "graph": graph})
        self.run_query(self.drivers[0], "CALL dbms.graph.createGraphWithRaft($name, $members)",
                       database="system", name=graph, members=members)
        with RawBolt(self.members[0]["bolt_port"]) as bolt:
            table = bolt.route(graph)
            roles = {server["role"]: server["addresses"] for server in table["servers"]}
            self.assertEqual(len(roles["ROUTE"]), 3)
            self.assertIn(f"[::1]:{members[2]['bolt_port']}", roles["ROUTE"])
            self.assertEqual(roles["READ"], [])
            self.assertEqual(roles["WRITE"], [])

    def test_05_driver_routes_each_graph_from_any_seed(self):
        self.assertNotEqual(self.leader("routing_a"), self.leader("routing_b"))
        for index, member in enumerate(self.members):
            with self.driver(member["bolt_port"]) as driver:
                for graph in ("routing_a", "routing_b"):
                    name = f"{graph}_{index}"
                    self.assertEqual(self.run_query(
                        driver, "CREATE (n:RoutingProbe {name: $name}) RETURN n.name AS name",
                        database=graph, name=name), [{"name": name}])
                    self.assertEqual(self.run_query(
                        driver, "MATCH (n:RoutingProbe {name: $name}) RETURN n.name AS name",
                        database=graph, read=True, name=name), [{"name": name}])

    def test_06_three_seeds_and_missing_database(self):
        def resolver(address):
            if address[0] == "cluster.test":
                return [("127.0.0.1", member["bolt_port"]) for member in self.members]
            return [address]

        with self.driver(self.members[0]["bolt_port"], host="cluster.test", resolver=resolver) as driver:
            self.assertEqual(self.run_query(driver, "RETURN 1 AS n"), [{"n": 1}])
            for graph in ("routing_a", "routing_b"):
                self.assertEqual(self.run_query(
                    driver, "CREATE (n:ResolverProbe {name: $name}) RETURN n.name AS name",
                    database=graph, name=graph), [{"name": graph}])
            with self.assertRaises(Neo4jError) as error:
                self.run_query(driver, "RETURN 1 AS n", database="missing_graph")
            self.assertEqual(error.exception.code, "Neo.ClientError.Database.DatabaseNotFound")

    def test_07_follower_failure_is_recognized(self):
        follower = self.leader("routing_a") % 3
        with self.assertRaises((NotALeader, WriteServiceUnavailable)) as error:
            self.run_query(self.drivers[follower], "CREATE (:RejectedRoutingProbe)")
        self.assertTrue(error.exception.is_retryable())
        # A routed connection must reject even a read sent to a stale writer.
        with RawBolt(self.members[follower]["bolt_port"], routing=True) as bolt:
            bolt.send(0x10, "RETURN 1 AS n", {}, {"db": "routing_a"})
            tag, fields = bolt.receive()
            self.assertEqual(tag, 0x7F)
            self.assertEqual(fields[0]["code"], "Neo.ClientError.Cluster.NotALeader")

    def test_08_driver_refreshes_after_leader_transfer(self):
        old_leader = self.leader("routing_a")
        with self.driver(self.members[0]["bolt_port"]) as driver, self.driver(
            self.members[0]["bolt_port"]
        ) as reader:
            self.assertEqual(self.run_query(driver, "RETURN 1 AS n"), [{"n": 1}])
            self.assertEqual(self.run_query(reader, "RETURN 1 AS n", read=True), [{"n": 1}])
            self.transfer("routing_a", old_leader % 3 + 1)
            try:
                self.run_query(driver, "RETURN 2 AS n")
            except NotALeader:
                pass
            self.assertEqual(self.run_query(driver, "RETURN 3 AS n"), [{"n": 3}])
            try:
                self.run_query(reader, "RETURN 2 AS n", read=True)
            except DatabaseUnavailable:
                pass
            self.assertEqual(self.run_query(reader, "RETURN 3 AS n", read=True), [{"n": 3}])

    def test_09_managed_transaction_apis(self):
        with self.driver(self.members[0]["bolt_port"]) as driver:
            for graph in ("routing_a", "routing_b", "default"):
                records, _, keys = driver.execute_query(
                    "RETURN 1 AS n", database_=graph)
                self.assertEqual(keys, ["n"])
                self.assertEqual([record.data() for record in records], [{"n": 1}])
                with driver.session(database=graph) as session:
                    self.assertEqual(session.execute_write(
                        lambda tx: tx.run("CREATE (n:ManagedTx {name: $name}) "
                                          "RETURN n.name AS name", name=graph).single()["name"]),
                                     graph)
                    self.assertEqual(session.execute_read(
                        lambda tx: tx.run("MATCH (n:ManagedTx {name: $name}) "
                                          "RETURN n.name AS name", name=graph).single()["name"]),
                                     graph)
            records, _, _ = driver.execute_query("CALL dbms.graph.listGraph()", database_="system")
            self.assertIn("routing_a", [record["name"] for record in records])

    def test_10_multi_statement_commit_and_rollback(self):
        with self.driver(self.members[0]["bolt_port"]) as driver:
            for graph in ("routing_a", "default"):
                with self.subTest(graph=graph), driver.session(database=graph) as session:
                    with session.begin_transaction() as tx:
                        tx.run("CREATE (:AtomicTx {name: 'committed', step: 1})").consume()
                        self.assertEqual(self.run_query(
                            driver, "MATCH (n:AtomicTx) RETURN count(n) AS n", database=graph),
                                         [{"n": 0}])
                        tx.run("CREATE (:AtomicTx {name: 'committed', step: 2})").consume()
                        self.assertEqual(tx.run("MATCH (n:AtomicTx) RETURN count(n) AS n").single()["n"], 2)
                        first_time = tx.run("RETURN datetime.transaction() AS t").single()["t"]
                        self.assertEqual(tx.run("RETURN datetime.transaction() AS t").single()["t"],
                                         first_time)
                        tx.commit()
                    self.assertEqual(self.run_query(
                        driver, "MATCH (n:AtomicTx) RETURN count(n) AS n", database=graph), [{"n": 2}])
                    with session.begin_transaction() as tx:
                        tx.run("CREATE (:AtomicTx {name: 'rolled_back', step: 1})").consume()
                        tx.run("CREATE (:AtomicTx {name: 'rolled_back', step: 2})").consume()
                        tx.rollback()
                    self.assertEqual(self.run_query(
                        driver, "MATCH (n:AtomicTx) RETURN count(n) AS n", database=graph), [{"n": 2}])

    def test_11_driver_handles_multiple_result_streams(self):
        with self.driver(self.members[0]["bolt_port"]) as driver:
            with driver.session(database="routing_a", fetch_size=1) as session:
                with session.begin_transaction() as tx:
                    first = tx.run("UNWIND range(1, 5) AS n RETURN n")
                    self.assertEqual(next(first)["n"], 1)
                    second = tx.run("UNWIND range(10, 14) AS n RETURN n")
                    self.assertEqual(next(second)["n"], 10)
                    self.assertEqual(first.value(), [2, 3, 4, 5])
                    self.assertEqual(second.value(), [11, 12, 13, 14])
                    # COMMIT discards all remaining records and still executes writes.
                    tx.run("UNWIND range(1, 4) AS n CREATE (:DiscardedTx {step: n}) RETURN n")
                    tx.commit()
            self.assertEqual(self.run_query(
                driver, "MATCH (n:DiscardedTx) RETURN count(n) AS n"), [{"n": 4}])

    def test_12_wire_qid_partial_pull_and_discard(self):
        with RawBolt(self.members[0]["bolt_port"]) as bolt:
            bolt.success(0x11, {})
            first = bolt.run("UNWIND range(1, 4) AS n RETURN n")["qid"]
            second = bolt.run("UNWIND range(10, 12) AS n RETURN n")["qid"]
            self.assertNotEqual(first, second)
            rows, metadata = bolt.stream(n=1, qid=first)
            self.assertEqual(rows, [[1]])
            self.assertTrue(metadata["has_more"])
            _, metadata = bolt.stream(n=1, qid=first, discard=True)
            self.assertTrue(metadata["has_more"])
            self.assertEqual(bolt.stream(qid=second)[0], [[10], [11], [12]])
            self.assertEqual(bolt.stream(qid=first)[0], [[3], [4]])
            bolt.success(0x12)
            # The latest qid is scoped to the new transaction.
            bolt.success(0x11, {})
            self.assertEqual(bolt.run("RETURN 7 AS n")["qid"], 0)
            self.assertEqual(bolt.stream()[0], [[7]])
            bolt.success(0x13)

    def test_13_failure_reset_and_disconnect_roll_back(self):
        for action in ("rollback", "reset", "disconnect", "failed_run", "failed_pull", "bad_qid"):
            with self.subTest(action=action):
                with RawBolt(self.members[0]["bolt_port"]) as bolt:
                    bolt.success(0x11, {})
                    bolt.run("CREATE (:AbortedTx {name: $name})", {"name": action})
                    bolt.stream(discard=True)
                    if action == "rollback":
                        bolt.success(0x13)
                    elif action in ("reset", "disconnect"):
                        bolt.run("UNWIND range(1, 5) AS n CREATE (:AbortedTx {name: $name}) "
                                 "RETURN n", {"name": action})
                        bolt.stream(n=1)
                        if action == "reset":
                            bolt.success(0x0F)
                    else:
                        if action == "failed_run":
                            bolt.send(0x10, "RETURN $missing", {}, {})
                        else:
                            bolt.run("RETURN 1 AS n" if action == "bad_qid"
                                     else "RETURN 1 / 0 AS n")
                            bolt.send(0x3F, {"n": -1, "qid": 100 if action == "bad_qid" else -1})
                        self.assertEqual(bolt.receive()[0], 0x7F)
                        bolt.send(0x12)
                        self.assertEqual(bolt.receive()[0], 0x7E)
                        bolt.success(0x0F)
                    if action != "disconnect":
                        bolt.run("RETURN 1 AS n")
                        self.assertEqual(bolt.stream()[0], [[1]])
                deadline = time.monotonic() + 4
                while self.run_query(self.drivers[0], "MATCH (n:AbortedTx {name: $name}) "
                                    "RETURN count(n) AS n", database="default", name=action) != [{"n": 0}]:
                    if time.monotonic() >= deadline:
                        self.fail(f"Writes survived {action}")
                    time.sleep(0.05)

    def test_14_read_only_and_administration_guards(self):
        with self.driver(self.members[0]["bolt_port"]) as driver:
            with driver.session(database="routing_a") as session:
                with self.assertRaises(Neo4jError) as error:
                    session.execute_read(lambda tx: tx.run("CREATE (:ReadOnlyTx)").consume())
                self.assertEqual(error.exception.code, "Neo.ClientError.Statement.AccessMode")
                with session.begin_transaction() as tx:
                    tx.run("CREATE (:AdminGuardTx)").consume()
                    with self.assertRaises(Neo4jError) as error:
                        tx.run("CALL db.index.createNodeIndex('tx_index', 'AdminGuardTx', ['name'], {})")
                    self.assertEqual(error.exception.code, "Unimplemented")
                    tx.rollback()
                self.assertEqual(self.run_query(driver, "MATCH (n:AdminGuardTx) RETURN count(n) AS n"),
                                 [{"n": 0}])
            with self.assertRaises(Neo4jError) as error:
                driver.execute_query("CALL dbms.graph.createGraph('tx_admin')", database_="system")
            self.assertEqual(error.exception.code, "Unimplemented")
            graphs = self.run_query(driver, "CALL dbms.graph.listGraph()", database="system")
            self.assertNotIn("tx_admin", [graph["name"] for graph in graphs])

    def test_15_invalid_transaction_metadata_and_database_switch(self):
        cases = [
            ((), "InputError"), ((None,), "InputError"),
            (({"db": 1},), "InputError"), (({"mode": "invalid"},), "InputError"),
            (({"bookmarks": None},), "InputError"), (({"bookmarks": [1]},), "InputError"),
            (({"bookmarks": ["bookmark"]},), "Unimplemented"),
            (({"tx_timeout": -1},), "InputError"), (({"tx_timeout": 5},), "Unimplemented"),
            (({"tx_metadata": None},), "InputError"),
            (({"imp_user": "other"},), "Unimplemented"),
            (({"db": "missing_graph"},), "Neo.ClientError.Database.DatabaseNotFound"),
        ]
        with RawBolt(self.members[0]["bolt_port"]) as bolt:
            for fields, code in cases:
                with self.subTest(fields=fields):
                    bolt.send(0x11, *fields)
                    tag, response = bolt.receive()
                    self.assertEqual(tag, 0x7F)
                    self.assertEqual(response[0]["code"], code)
                    bolt.success(0x0F)
            for extra in ({"db": "routing_b"}, {"mode": "r"}):
                bolt.success(0x11, {})
                bolt.run("CREATE (:SwitchGuardTx)")
                bolt.stream(discard=True)
                bolt.send(0x10, "RETURN 1 AS n", {}, extra)
                self.assertEqual(bolt.receive()[0], 0x7F)
                bolt.success(0x0F)
            self.assertEqual(self.run_query(self.drivers[0],
                "MATCH (n:SwitchGuardTx) RETURN count(n) AS n", database="default"), [{"n": 0}])

    def test_16_managed_transactions_retry_after_leader_transfer(self):
        for mode in ("write", "read", "new_term"):
            with self.subTest(mode=mode):
                attempts = []
                old_leader = self.leader("routing_a")
                with self.driver(self.members[0]["bolt_port"], max_transaction_retry_time=15) as driver:
                    with driver.session(database="routing_a") as session:
                        def work(tx):
                            attempts.append(len(attempts) + 1)
                            if mode != "read":
                                tx.run("CREATE (:RetryTx {step: 1, mode: $mode})", mode=mode).consume()
                                tx.run("CREATE (:RetryTx {step: 2, mode: $mode})", mode=mode).consume()
                            else:
                                tx.run("RETURN 1 AS n").consume()
                            if len(attempts) == 1:
                                self.transfer("routing_a", old_leader % 3 + 1)
                                if mode == "new_term":
                                    self.transfer("routing_a", old_leader)
                            # Fail at COMMIT after all statements have been consumed.
                            return len(attempts)

                        result = (session.execute_read if mode == "read" else session.execute_write)(work)
                        self.assertEqual(result, 2)
                        self.assertEqual(len(attempts), 2)
                    if mode != "read":
                        self.assertEqual(self.run_query(driver,
                            "MATCH (n:RetryTx {mode: $mode}) RETURN n.step AS step ORDER BY step", mode=mode),
                                         [{"step": 1}, {"step": 2}])

    def test_17_reset_interrupts_running_query(self):
        for explicit in (False, True):
            with self.subTest(explicit=explicit), RawBolt(self.members[0]["bolt_port"]) as bolt:
                if explicit:
                    bolt.success(0x11, {})
                bolt.run("UNWIND range(1, 1000) AS n UNWIND range(1, 1000) AS m "
                         "CREATE (:InterruptedTx) RETURN n")
                bolt.send(0x3F, {"n": -1})
                # The write barrier drains writes before producing any record.
                # Interrupt while that work is still inside cursor.Next().
                time.sleep(0.05)
                bolt.send(0x0F)
                while True:
                    tag, _ = bolt.receive()
                    if tag != 0x71:
                        self.assertEqual(tag, 0x7E)
                        break
                self.assertEqual(bolt.receive()[0], 0x70)
                bolt.run("RETURN 1 AS n")
                self.assertEqual(bolt.stream()[0], [[1]])
                self.assertEqual(self.run_query(self.drivers[0],
                    "MATCH (n:InterruptedTx) RETURN count(n) AS n", database="default"), [{"n": 0}])

    def test_18_disconnect_rolls_back_idle_and_running_transactions(self):
        for mode in ("idle", "running"):
            with self.subTest(mode=mode):
                self.run_query(self.drivers[0],
                    "CREATE (:DisconnectedTx {name: $name, value: 0})", database="default", name=mode)
                with RawBolt(self.members[0]["bolt_port"]) as bolt:
                    bolt.success(0x11, {})
                    bolt.run("MATCH (n:DisconnectedTx {name: $name}) SET n.value = 1", {"name": mode})
                    bolt.stream(discard=True)
                    if mode == "running":
                        bolt.run("UNWIND range(1, 1000) AS n UNWIND range(1, 1000) AS m "
                                 "CREATE (:DisconnectedTx {name: 'aborted'}) RETURN n")
                        bolt.send(0x3F, {"n": -1})
                        time.sleep(0.05)
                deadline = time.monotonic() + 4
                while True:
                    try:
                        # Reuse an existing pooled connection so accepting a new
                        # connection cannot trigger cleanup of the closed one.
                        rows = self.run_query(self.drivers[0],
                            "MATCH (n:DisconnectedTx {name: $name}) "
                            "SET n.value = n.value + 2 RETURN n.value AS value",
                            database="default", name=mode)
                        self.assertEqual(rows, [{"value": 2}])
                        break
                    except Neo4jError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.05)
                self.assertEqual(self.run_query(self.drivers[0],
                    "MATCH (n:DisconnectedTx {name: 'aborted'}) RETURN count(n) AS n", database="default"),
                                 [{"n": 0}])

    def test_99_existing_driver_recovers_after_leader_stops(self):
        old_leader = self.leader("routing_a")
        with self.driver(self.members[old_leader - 1]["bolt_port"]) as driver:
            self.assertEqual(self.run_query(driver, "RETURN 1 AS n"), [{"n": 1}])
            self.processes[old_leader - 1].terminate()
            self.processes[old_leader - 1].wait(timeout=5)
            new_leader = self.leader("routing_a")
            self.assertNotEqual(old_leader, new_leader)
            deadline = time.monotonic() + 15
            while True:
                try:
                    rows = self.run_query(
                        driver, "MERGE (n:FailoverProbe {name: 'once'}) RETURN n.name AS name")
                    self.assertEqual(rows, [{"name": "once"}])
                    break
                except (DriverError, Neo4jError) as error:
                    if not error.is_retryable() or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.1)
        # Bootstrap must also work when the first resolved seed is down.
        def resolver(address):
            if address[0] == "cluster.test":
                order = [old_leader - 1] + [i for i in range(3) if i != old_leader - 1]
                return [("127.0.0.1", self.members[i]["bolt_port"]) for i in order]
            return [address]

        with self.driver(self.members[old_leader - 1]["bolt_port"],
                         host="cluster.test", resolver=resolver) as fresh_driver:
            self.assertEqual(self.run_query(fresh_driver, "RETURN 1 AS n"), [{"n": 1}])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args, rest = parser.parse_known_args()
    SERVER = str(Path(args.server).resolve())
    logging.getLogger("neo4j").setLevel(logging.CRITICAL)
    unittest.main(argv=[__file__, *rest], verbosity=2)
