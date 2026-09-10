//
// Created by botu.wzy
//

#pragma once
#include <rocksdb/utilities/transaction_db.h>

#include <unordered_map>
namespace graphdb {
struct GraphCF {
  rocksdb::ColumnFamilyHandle* graph_topology = nullptr;
  rocksdb::ColumnFamilyHandle* vertex_property = nullptr;
  rocksdb::ColumnFamilyHandle* vertex_vector_property = nullptr;
  rocksdb::ColumnFamilyHandle* edge_property = nullptr;
  rocksdb::ColumnFamilyHandle* vertex_label_vid = nullptr;
  rocksdb::ColumnFamilyHandle* edge_type_eid = nullptr;
  rocksdb::ColumnFamilyHandle* meta_info = nullptr;
  rocksdb::ColumnFamilyHandle* index = nullptr;
  rocksdb::ColumnFamilyHandle* wal = nullptr;
};
}  // namespace graphdb
