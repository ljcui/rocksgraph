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
- **Not Distributed**: It is not a distributed database.

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

By default the server listens on Bolt port `7687` (Raft on `7688`).

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
