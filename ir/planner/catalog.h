#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ir {

class PlannerCatalog {
 public:
  PlannerCatalog() = default;
  PlannerCatalog(const PlannerCatalog &) = delete;
  PlannerCatalog &operator=(const PlannerCatalog &) = delete;
  virtual ~PlannerCatalog() = default;

  [[nodiscard]] virtual bool HasNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const = 0;
  [[nodiscard]] virtual bool HasRelationshipIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const = 0;
};

class HeuristicPlannerCatalog final : public PlannerCatalog {
 public:
  [[nodiscard]] bool HasNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override {
    (void)labels;
    return !property_key.empty();
  }

  [[nodiscard]] bool HasRelationshipIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const override {
    (void)relationship_types;
    return !property_key.empty();
  }
};

}  // namespace ir
