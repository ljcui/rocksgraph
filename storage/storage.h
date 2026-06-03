#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "storage/access_path.h"
#include "value/value.h"

namespace rg {

class Storage : public AccessPath {
 public:
  using NodePtr = Value::NodePtr;
  using RelationshipPtr = Value::RelationshipPtr;

  Storage() = default;
  Storage(const Storage &) = delete;
  Storage &operator=(const Storage &) = delete;
  ~Storage() override = default;

  virtual NodePtr CreateNode(std::vector<std::string> labels,
                             Value::Map properties = {}) = 0;
  virtual RelationshipPtr CreateRelationship(std::int64_t start_node_id,
                                             std::int64_t end_node_id,
                                             std::string type,
                                             Value::Map properties = {}) = 0;

  virtual void SetNodeProperty(std::int64_t node_id, std::string property_key,
                               Value value) = 0;
  virtual void SetRelationshipProperty(std::int64_t relationship_id,
                                       std::string property_key,
                                       Value value) = 0;
  virtual void SetNodeProperties(std::int64_t node_id, Value::Map properties,
                                 bool include_existing) = 0;
  virtual void SetLabels(std::int64_t node_id,
                         std::vector<std::string> labels) = 0;
  virtual void RemoveNodeProperty(std::int64_t node_id,
                                  std::string_view property_key) = 0;
  virtual void RemoveRelationshipProperty(std::int64_t relationship_id,
                                          std::string_view property_key) = 0;
  virtual void RemoveLabels(std::int64_t node_id,
                            const std::vector<std::string> &labels) = 0;
  virtual void DeleteNode(std::int64_t node_id) = 0;
  virtual void DeleteRelationship(std::int64_t relationship_id) = 0;
};

}  // namespace rg
