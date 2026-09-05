#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ir/logical_plan.h"
#include "runtime/slotted_row.h"

namespace rg {

using OperatorId = std::size_t;

struct PhysicalPlanNode {
  OperatorId id = 0;
  const ir::LogicalPlan *logical = nullptr;
  SlotConfigurationPtr argument_slots;
  SlotConfigurationPtr output_slots;
  std::vector<std::vector<SlotMapping>> child_mappings;
  std::vector<std::unique_ptr<PhysicalPlanNode>> children;
  std::size_t value_hash_join_build_child = 1;
};

class PhysicalPlan final {
 public:
  explicit PhysicalPlan(std::unique_ptr<PhysicalPlanNode> root);

  [[nodiscard]] const PhysicalPlanNode &Root() const;
  [[nodiscard]] const PhysicalPlanNode &NodeFor(
      const ir::LogicalPlan &logical) const;

 private:
  void Index(const PhysicalPlanNode &node);

  std::unique_ptr<PhysicalPlanNode> root_;
  std::unordered_map<const ir::LogicalPlan *, const PhysicalPlanNode *> nodes_;
};

[[nodiscard]] PhysicalPlan CreatePhysicalPlan(const ir::LogicalPlan &plan);
[[nodiscard]] bool PlanContainsWrites(const ir::LogicalPlan &plan);

}  // namespace rg
