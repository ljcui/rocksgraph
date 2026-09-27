# Built-in Procedures

rocksgraph ships with a set of built-in procedures that can be called from
OpenCypher with the `CALL` clause. Procedure names are case-insensitive, and
result fields are consumed with `YIELD`:

```cypher
CALL db.labels() YIELD label WHERE label = 'Person' RETURN label;
```

`YIELD *` returns all fields. The full catalog is also self-describing:

```cypher
CALL dbms.procedures() YIELD name, signature, description, mode, worksOnSystem;
```

## Summary

| Procedure | Mode | Description |
|---|---|---|
| `db.labels()` | read | List all node labels in the graph |
| `db.propertyKeys()` | read | List all property keys in the graph |
| `db.relationshipTypes()` | read | List all relationship types in the graph |
| `dbms.procedures()` | read | List all built-in procedures |
| `dbms.graph.getRaftNodeInfos(graph_name)` | read | List Raft nodes and identify the current leader |
| `db.index.createNodeIndex(index_name, label, properties, parameter)` | write | Create a node property index |
| `db.index.queryNodes(index_name, query)` | read | Query nodes by an exact property index key |
| `db.index.rangeQueryNodes(index_name, lower, upper, parameter)` | read | Query nodes by a property index range |
| `db.index.fulltext.createNodeIndex(index_name, labels, properties)` | write | Create a node full-text index |
| `db.index.fulltext.queryNodes(index_name, query, top_n)` | read | Query nodes by a full-text index |
| `db.index.vector.createNodeField(label, property, parameter)` | write | Define a node vector field |
| `db.index.vector.createNodeIndex(index_name, label, property, parameter)` | write | Create a node vector index |
| `db.index.vector.knnSearchNodes(index_name, query, parameter)` | read | Query nearest nodes by a vector index |

## Catalog

### `db.labels() :: (label)`

Lists all node labels in the graph.

```cypher
CALL db.labels() YIELD label;
```

### `db.propertyKeys() :: (propertyKey)`

Lists all property keys in the graph.

```cypher
CALL db.propertyKeys() YIELD propertyKey AS key RETURN key ORDER BY key;
```

### `db.relationshipTypes() :: (relationshipType)`

Lists all relationship types in the graph.

```cypher
CALL db.relationshipTypes() YIELD relationshipType AS type;
```

### `dbms.procedures() :: (name, signature, description, mode, worksOnSystem)`

Lists all built-in procedures with their signature, access mode
(`READ` or `WRITE`), and whether they work on the system database.

```cypher
CALL dbms.procedures();
```

### `dbms.graph.getRaftNodeInfos(graph_name) :: (node_id, ip, bolt_port, raft_port, is_leader)`

Lists the Raft nodes of the given graph and identifies the current leader.

```cypher
CALL dbms.graph.getRaftNodeInfos('default')
YIELD node_id, ip, bolt_port, raft_port, is_leader;
```

## Property Indexes

### `db.index.createNodeIndex(index_name, label, properties, parameter) :: ()`

Creates a node property index. `properties` is a list of property names; an
index over more than one property is a composite index.

Options (`parameter` map):

| Option | Type | Default | Description |
|---|---|---|---|
| `unique` | boolean | `false` | Create a unique (uniqueness-constrained) index |

```cypher
CALL db.index.createNodeIndex(
    'person_region_score', 'Person', ['region', 'score'], {unique: false});
```

### `db.index.queryNodes(index_name, query) :: (node)`

Queries nodes by an exact property index key. For a composite index, `query`
is a list with one value per indexed property, in index order.

```cypher
CALL db.index.queryNodes('person_region_score', ['north', 30])
YIELD node RETURN node.name;
```

### `db.index.rangeQueryNodes(index_name, lower, upper, parameter) :: (node)`

Queries nodes by a property index range. `lower` and `upper` bound the key of
a (composite) index and may be `null` for an open-ended side. Like `query` in
`db.index.queryNodes`, each bound is a list for a composite index.

Options (`parameter` map):

| Option | Type | Default | Description |
|---|---|---|---|
| `left_closed` | boolean | `true` | Include `lower` in the range |
| `right_closed` | boolean | `true` | Include `upper` in the range |

```cypher
CALL db.index.rangeQueryNodes('person_region_score', ['north', 10],
                              ['north', 30],
                              {left_closed: true, right_closed: false})
YIELD node RETURN node.name ORDER BY node.name;
```

## Full-Text Indexes

### `db.index.fulltext.createNodeIndex(index_name, labels, properties) :: ()`

Creates a node full-text index. `labels` and `properties` are lists.

```cypher
CALL db.index.fulltext.createNodeIndex('person_text', ['Person'], ['name']);
```

### `db.index.fulltext.queryNodes(index_name, query, top_n) :: (node, score)`

Queries nodes by a full-text index, returning the top `top_n` matches with
their relevance `score`.

```cypher
CALL db.index.fulltext.queryNodes('person_text', 'Carol', 10)
YIELD node, score RETURN node.name, score;
```

## Vector Indexes

### `db.index.vector.createNodeField(label, property, parameter) :: ()`

Defines a vector field on nodes with the given label, so that vector values
can be stored in the property.

Options (`parameter` map):

| Option | Type | Default | Description |
|---|---|---|---|
| `dimension` | integer | required | Dimension of the vectors |

```cypher
CALL db.index.vector.createNodeField('Person', 'embedding', {dimension: 2});
```

### `db.index.vector.createNodeIndex(index_name, label, property, parameter) :: ()`

Creates a node vector index (HNSW) over a vector field.

Options (`parameter` map):

| Option | Type | Default | Description |
|---|---|---|---|
| `dimension` | integer | required | Dimension of the vectors |
| `distance_type` | string | `'l2'` | Distance metric: `'l2'` (Euclidean) or `'ip'` (inner product) |
| `hnsw_m` | integer | `16` | Maximum number of connections per HNSW layer |
| `hnsw_ef_construction` | integer | `100` | Size of the candidate list during index construction |

```cypher
CALL db.index.vector.createNodeIndex('person_vector', 'Person', 'embedding',
    {dimension: 2, distance_type: 'l2', hnsw_m: 8, hnsw_ef_construction: 20});
```

### `db.index.vector.knnSearchNodes(index_name, query, parameter) :: (node, distance)`

Queries the `top_k` nearest nodes to a query vector, returning each node with
its `distance`.

Options (`parameter` map):

| Option | Type | Default | Description |
|---|---|---|---|
| `top_k` | integer | `10` | Number of nearest neighbors to return |
| `ef_search` | integer | `200` | Size of the candidate list during search |

```cypher
CALL db.index.vector.knnSearchNodes('person_vector', [1.0, 0.0], {top_k: 2})
YIELD node, distance RETURN node.name, distance ORDER BY distance;
```

## Notes

- Index creation procedures (`db.index.createNodeIndex`,
  `db.index.fulltext.createNodeIndex`, `db.index.vector.createNodeField`,
  `db.index.vector.createNodeIndex`) build their index asynchronously: the call
  returns immediately, and queries against the index are only valid once the
  index becomes ready.
- Publishing or dropping a GraphDB property index advances the catalog
  version and invalidates cached plans.
