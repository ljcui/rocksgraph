#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ir/logical_plan.h"
#include "runtime/slotted_row.h"

namespace rg {

using OperatorId = std::size_t;

enum class PhysicalOperatorType {
  kLogical,
  kFullSort,
  kPartialSort,
  kHashDistinct,
  kOrderedDistinct,
  kHashAggregation,
  kOrderedAggregation,
  kTopN,
};

[[nodiscard]] std::string_view ToString(PhysicalOperatorType type);

struct PhysicalPlanNode {
  OperatorId id = 0;
  PhysicalOperatorType type = PhysicalOperatorType::kLogical;
  const ir::LogicalPlan *logical = nullptr;
  const ir::SortPlan *top_n_sort = nullptr;
  SlotConfigurationPtr argument_slots;
  SlotConfigurationPtr output_slots;
  std::vector<ir::LogicalSortItem> provided_order;
  std::vector<std::vector<SlotMapping>> child_mappings;
  std::vector<std::unique_ptr<PhysicalPlanNode>> children;
  std::size_t partial_sort_prefix = 0;
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
