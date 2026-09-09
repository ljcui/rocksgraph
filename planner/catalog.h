#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

struct NodeIndexDescriptor {
  std::string property_key;
  bool unique = false;
  bool supports_equality = true;
  bool supports_range = true;
};

struct RelationshipIndexDescriptor {
  std::string property_key;
  bool unique = false;
  bool supports_equality = true;
  bool supports_range = true;
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

class EmptyPlannerCatalog final : public PlannerCatalog {
 public:
  [[nodiscard]] std::optional<NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override {
    (void)labels;
    (void)property_key;
    return std::nullopt;
  }

  [[nodiscard]] std::optional<RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const override {
    (void)relationship_types;
    (void)property_key;
    return std::nullopt;
  }
};

}  // namespace ir
