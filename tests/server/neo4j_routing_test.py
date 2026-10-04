"""Exercise Bolt routing with the official driver and a real Raft cluster."""

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
        return GraphDatabase.driver(
            f"{scheme}://{host}:{port}", auth=("neo4j", "password"),
            connection_timeout=1, connection_acquisition_timeout=4,
            max_transaction_retry_time=0, **kwargs,
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

    def test_09_explicit_transactions_still_report_unsupported(self):
        with self.driver(self.members[0]["bolt_port"]) as driver:
            with self.assertRaises(Neo4jError) as error:
                driver.execute_query("RETURN 1 AS n", database_="routing_a")
            self.assertEqual(error.exception.code, "Unimplemented")

    def test_10_existing_driver_recovers_after_leader_stops(self):
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
