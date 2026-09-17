#include "runtime/graphdb_planner_catalog.h"

#include "graphdb/graph_db.h"

namespace rg {

std::optional<planner::NodeIndexDescriptor>
GraphDBPlannerCatalog::FindNodeIndex(const std::vector<std::string>& labels,
                                     std::string_view property_key) const {
  if (labels.size() != 1 || property_key.empty()) {
    return std::nullopt;
  }

  auto lid = graph_->id_generator().GetLid(labels.front());
  auto pid = graph_->id_generator().GetPid(std::string(property_key));
  if (!lid.has_value() || !pid.has_value()) {
    return std::nullopt;
  }
  auto index = graph_->meta_info().GetReadyVertexPropertyIndex(*lid, *pid);
  if (!index || index->PropertyCount() != 1) {
    return std::nullopt;
  }
  return planner::NodeIndexDescriptor{.property_key = std::string(property_key),
                                      .unique = index->is_unique()};
}

std::optional<planner::RelationshipIndexDescriptor>
GraphDBPlannerCatalog::FindRelationshipIndex(
    const std::vector<std::string>& relationship_types,
    std::string_view property_key) const {
  if (relationship_types.size() != 1 || property_key.empty()) {
    return std::nullopt;
  }

  auto tid = graph_->id_generator().GetTid(relationship_types.front());
  auto pid = graph_->id_generator().GetPid(std::string(property_key));
  if (!tid.has_value() || !pid.has_value()) {
    return std::nullopt;
  }
  auto index = graph_->meta_info().GetReadyEdgePropertyIndex(*tid, *pid);
  if (!index || index->PropertyCount() != 1) {
    return std::nullopt;
  }
  return planner::RelationshipIndexDescriptor{
      .property_key = std::string(property_key), .unique = index->is_unique()};
}

}  // namespace rg
