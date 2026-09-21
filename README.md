# RocksGraph

RocksGraph is a graph database written in C++, built on RocksDB storage, with OpenCypher as its query language.

## Features

- **Free Schema**: No predefined schema required — add properties on the fly.
- **OpenCypher**: Uses OpenCypher as the query language, with full syntax support.
- **Multiple Graphs**: Supports multiple subgraphs, physically isolated from each other.
- **Transactions**: ACID transaction support.
- **Large-Scale Persistent Storage**: Built on RocksDB for large datasets and durable persistence.
- **Replication**: Replicas synchronize data via the Raft protocol for high availability.
- **Neo4j Client Compatible**: Implements the Bolt protocol, so Neo4j clients can connect directly.
- **Rich Indexes**: Supports property indexes, full-text indexes, and vector indexes.
- **Single-Node**: A single-node database, not distributed.
