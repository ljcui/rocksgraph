#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "planner/catalog.h"

namespace graphdb {
class GraphDB;
}

namespace rg {

class GraphDBPlannerCatalog final : public ir::PlannerCatalog {
 public:
  explicit GraphDBPlannerCatalog(graphdb::GraphDB& graph) : graph_(&graph) {}

  [[nodiscard]] std::optional<ir::NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string>& labels,
      std::string_view property_key) const override;

  [[nodiscard]] std::optional<ir::RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string>& relationship_types,
                        std::string_view property_key) const override;

 private:
  graphdb::GraphDB* graph_ = nullptr;
};

}  // namespace rg
