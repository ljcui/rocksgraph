# rocksgraph

rocksgraph is a graph database written in C++, with OpenCypher as its query language.

## Features

- **Free Schema**: No predefined schema required — add properties on the fly.
- **OpenCypher**: Uses OpenCypher as the query language, with full syntax support.
- **Multiple Graphs**: Supports multiple subgraphs, physically isolated from each other.
- **Transactions**: ACID transaction support.
- **Persistent Storage**: Data is durably persisted on disk, supporting large datasets.
- **Replication**: Replicas synchronize data via the Raft protocol for high availability.
- **Neo4j Client Compatible**: Implements the Bolt protocol, so Neo4j clients can connect directly.
- **Rich Indexes**: Supports property indexes, full-text indexes, and vector indexes.
- **Client-Server**: It is not an embedded database; it runs as a server and is accessed over the network.
- **No Sharding**: Raft replicates whole graphs; data sharding is not supported.

## Build

Build the compiler image with `docker/ubuntu_24.04.dockerfile` and compile the source code inside it:

```bash
git clone https://github.com/ljcui/rocksgraph.git && cd rocksgraph
mkdir build && cd build
cmake .. && make -j8
```

## Quick Start

The build produces the database server `build/server/rg-server` and its interactive Bolt client `build/tools/rg-cli`. Start the server:

```bash
./build/server/rg-server --data_path=./data
```

By default the server listens on Bolt port `7687`, Raft port `7688`, and HTTP snapshot transfer port `7689`.

Connect and query from another terminal with `rg-cli`:

```bash
./build/tools/rg-cli
```

```cypher
CREATE (n:Person {name: 'Alice'});
MATCH (n:Person) RETURN n.name;
```

Any Neo4j client also works, since rocksgraph speaks the Bolt protocol. For example, with the Python client:

```python
from neo4j import GraphDatabase

driver = GraphDatabase.driver("bolt://localhost:7687", auth=("neo4j", "password"))
with driver.session(database="default") as session:
    session.run("CREATE (n:Person {name: 'Alice'})")
    for record in session.run("MATCH (n:Person) RETURN n.name"):
        print(record["n.name"])
driver.close()
```

## Multiple Graphs

The server creates a local `default` graph on first startup. The reserved `system` database exposes graph management procedures. Connect to it to create and list graphs:

```bash
./build/tools/rg-cli --graph=system
```

```cypher
CALL dbms.graph.createGraph('people');
CALL dbms.graph.listGraph() YIELD id, name RETURN id, name;
```

Reconnect with the graph name to store and query its data:

```bash
./build/tools/rg-cli --graph=people
```

```cypher
CREATE (:Person {name: 'Alice'});
MATCH (p:Person) RETURN p.name AS name;
```

With the Python driver, select the graph with `driver.session(database="people")`. Graph management procedures must run as auto-commit queries, for example with `session.run(...).consume()` on `system`.

## Transactions

Each `session.run()` starts an auto-commit transaction. It commits when its result stream is fully consumed or discarded; call `.consume()` when no records are needed.

Use an explicit transaction to commit multiple data queries together. This example uses the `people` graph created above:

```python
from neo4j import GraphDatabase

with GraphDatabase.driver("bolt://localhost:7687", auth=("neo4j", "password")) as driver:
    with driver.session(database="people") as session:
        with session.begin_transaction() as tx:
            tx.run("CREATE (:Person {name: $name})", name="Bob").consume()
            tx.run(
                "MATCH (p:Person {name: $name}) SET p.active = true",
                name="Bob",
            ).consume()
            tx.commit()
```

Call `tx.rollback()` to abort; closing an uncommitted transaction also rolls it back. Consume all result streams before committing or rolling back. The graph and access mode are fixed for the transaction's lifetime, and read access mode rejects writes. Graph management and index-creation procedures must run outside explicit or managed transactions.

## Indexes

On the `people` graph, create a property index using an auto-commit query:

```cypher
CALL db.index.createNodeIndex('person_name', 'Person', ['name'], {unique: false});
```

Indexes build asynchronously, so the creation call can return before the index is ready. After the build finishes, query it by an exact property value:

```cypher
CALL db.index.queryNodes('person_name', 'Alice')
YIELD node RETURN node.name AS name;
```

A query made while the index is still building returns `IndexNotReady`; retry after the build completes. See [Built-in Procedures](docs/builtin_procedures.md) for composite and range property indexes, full-text search, and vector search examples and options.

## Raft Cluster

Raft replicates each graph in full across its members; data sharding is not supported. Replication is enabled per graph, and the automatically created `default` graph is local to each server.

To run a three-node cluster on one machine, start each command in a separate terminal from the repository root. Each process needs its own data and log directories, node ID, and ports:

```bash
# Terminal 1
./build/server/rg-server --data_path=./cluster/node1/data --log_path=./cluster/node1/log \
    --host=127.0.0.1 --raft_node_id=1 \
    --bolt_port=17687 --raft_port=17688 --http_port=17689

# Terminal 2
./build/server/rg-server --data_path=./cluster/node2/data --log_path=./cluster/node2/log \
    --host=127.0.0.1 --raft_node_id=2 \
    --bolt_port=27687 --raft_port=27688 --http_port=27689

# Terminal 3
./build/server/rg-server --data_path=./cluster/node3/data --log_path=./cluster/node3/log \
    --host=127.0.0.1 --raft_node_id=3 \
    --bolt_port=37687 --raft_port=37688 --http_port=37689
```

Create the Raft graph on **every node** through its local `system` database, using the same graph name and member list. Run this Python script once for the new cluster:

```python
from neo4j import GraphDatabase

graph_name = "cluster_demo"
members = [
    {"node_id": 1, "ip": "127.0.0.1", "bolt_port": 17687, "raft_port": 17688, "graph": graph_name},
    {"node_id": 2, "ip": "127.0.0.1", "bolt_port": 27687, "raft_port": 27688, "graph": graph_name},
    {"node_id": 3, "ip": "127.0.0.1", "bolt_port": 37687, "raft_port": 37688, "graph": graph_name},
]

for member in members:
    uri = f"bolt://{member['ip']}:{member['bolt_port']}"
    with GraphDatabase.driver(uri, auth=("neo4j", "password")) as driver:
        with driver.session(database="system") as session:
            session.run(
                "CALL dbms.graph.createGraphWithRaft($name, $members)",
                name=graph_name,
                members=members,
            ).consume()
```

Each member's `graph` must match the graph name, and its `node_id`, `bolt_port`, and `raft_port` must match that server's flags. For deployment across machines, replace the loopback addresses in both `--host` and the member list with addresses reachable by peers and clients, and make the Bolt, Raft, and HTTP snapshot ports reachable.

Wait for a leader to be elected. Connect to a node's `system` database to inspect the cluster:

```bash
./build/tools/rg-cli --ip=127.0.0.1 --port=17687 --graph=system
```

```cypher
CALL dbms.graph.getRaftNodeInfos('cluster_demo')
YIELD node_id, ip, bolt_port, raft_port, is_leader;
```

Once a node reports `is_leader = true`, connect with `neo4j://` and specify the Raft graph name to enable routing:

```python
from neo4j import GraphDatabase

with GraphDatabase.driver("neo4j://127.0.0.1:17687", auth=("neo4j", "password")) as driver:
    with driver.session(database="cluster_demo") as session:
        session.execute_write(
            lambda tx: tx.run("CREATE (:Person {name: 'Alice'})").consume()
        )
        names = session.execute_read(
            lambda tx: [record["name"] for record in tx.run(
                "MATCH (p:Person) RETURN p.name AS name"
            )]
        )
        print(names)
```

The routing table lists all members as routers and the current leader as both reader and writer. Managed transactions (`execute_read` / `execute_write`) allow the driver to retry when leadership changes. The CLI uses a direct Bolt connection; connect it to the current leader to write to a Raft graph.

## Configuration

Pass server options on the command line. Common options and their defaults are:

| Option | Default | Description |
|---|---|---|
| `--data_path` | `data` | Directory for graph data and metadata |
| `--log_path` | `log` | Directory for server logs |
| `--log_level` | `info` | Server log level |
| `--host` | `0.0.0.0` | Local node address used by Raft; also the HTTP snapshot bind address |
| `--bolt_port` | `7687` | Bolt client port |
| `--raft_port` | `7688` | Raft peer communication port |
| `--http_port` | `7689` | HTTP port for Raft snapshot transfer |
| `--raft_node_id` | `1` | Positive node ID, unique within each cluster |
| `--graph_block_cache` | `8589934592` (8 GiB) | Graph data block cache shared by all graphs, in bytes |
| `--raft_log_block_cache` | `268435456` (256 MiB) | Raft log block cache shared by all graphs, in bytes |
| `--plan_cache_capacity` | `1024` | Maximum compiled plans cached across graphs; `0` disables caching |

Relative paths are resolved from the server's working directory. For a Raft cluster, set `--host` to a reachable node address. List all server options with:

```bash
./build/server/rg-server --help
```
