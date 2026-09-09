#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "planner/catalog.h"

namespace test_support {

class AssumeAllIndexesPlannerCatalog final : public ir::PlannerCatalog {
 public:
  [[nodiscard]] std::optional<ir::NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override {
    (void)labels;
    if (property_key.empty()) {
      return std::nullopt;
    }
    return ir::NodeIndexDescriptor{.property_key = std::string(property_key)};
  }

  [[nodiscard]] std::optional<ir::RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const override {
    (void)relationship_types;
    if (property_key.empty()) {
      return std::nullopt;
    }
    return ir::RelationshipIndexDescriptor{.property_key =
                                               std::string(property_key)};
  }
};

inline const AssumeAllIndexesPlannerCatalog &AssumeAllIndexesCatalog() {
  static const AssumeAllIndexesPlannerCatalog catalog;
  return catalog;
}

}  // namespace test_support
