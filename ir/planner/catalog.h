#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

struct NodeIndexDescriptor {
  std::string property_key;
  bool unique = false;
};

struct RelationshipIndexDescriptor {
  std::string property_key;
  bool unique = false;
};

class PlannerCatalog {
 public:
  PlannerCatalog() = default;
  PlannerCatalog(const PlannerCatalog &) = delete;
  PlannerCatalog &operator=(const PlannerCatalog &) = delete;
  virtual ~PlannerCatalog() = default;

  [[nodiscard]] virtual std::optional<NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const = 0;
  [[nodiscard]] virtual std::optional<RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const = 0;
};

class HeuristicPlannerCatalog final : public PlannerCatalog {
 public:
  [[nodiscard]] std::optional<NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override {
    (void)labels;
    if (property_key.empty()) {
      return std::nullopt;
    }
    return NodeIndexDescriptor{.property_key = std::string(property_key),
                               .unique = false};
  }

  [[nodiscard]] std::optional<RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const override {
    (void)relationship_types;
    if (property_key.empty()) {
      return std::nullopt;
    }
    return RelationshipIndexDescriptor{
        .property_key = std::string(property_key), .unique = false};
  }
};

}  // namespace ir
