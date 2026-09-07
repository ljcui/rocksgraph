#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ast/ast_node.h"
#include "ir/logical_plan.h"
#include "runtime/slotted_row.h"

namespace rg {

using OperatorId = std::size_t;

enum class PhysicalOperatorKind {
  kArgument,
  kAllNodeScan,
  kNodeByLabelScan,
  kNodeIndexSeek,
  kNodeIndexRangeSeek,
  kRelationshipTypeScan,
  kRelationshipIndexSeek,
  kRelationshipIndexRangeSeek,
  kExpand,
  kExpandInto,
  kVarExpand,
  kPathBuild,
  kFilter,
  kProjection,
  kHashDistinct,
  kOrderedDistinct,
  kHashAggregation,
  kOrderedAggregation,
  kFullSort,
  kPartialSort,
  kSkip,
  kLimit,
  kTopN,
  kPartialTopN,
  kProduceResults,
  kCartesianProduct,
  kNodeHashJoin,
  kValueHashJoin,
  kPredicateJoin,
  kApply,
  kSemiApply,
  kAntiSemiApply,
  kLetSemiApply,
  kRollUpApply,
  kOptionalApply,
  kAssertIsNode,
  kWriteBarrier,
  kCreateNode,
  kCreateRelationship,
  kMerge,
  kSetProperty,
  kSetProperties,
  kSetLabels,
  kRemoveProperty,
  kRemoveLabels,
  kDelete,
  kDetachDelete,
  kUnwind,
  kProcedureCall,
  kNodeByIdSeek,
  kRelationshipByIdSeek,
  kProjectEndpoints,
  kOptionalExpand,
  kLeftOuterHashJoin,
  kSelectOrSemiApply,
  kPruningVarExpand,
  kUnion,
};

[[nodiscard]] std::string_view ToString(PhysicalOperatorKind kind);

struct PhysicalPrecomputedExpression {
  std::unique_ptr<ast::Expression> expression;
  std::string variable;
};

class PhysicalExpression final {
 public:
  PhysicalExpression() = default;
  PhysicalExpression(
      std::unique_ptr<ast::Expression> expression,
      std::vector<PhysicalPrecomputedExpression> precomputed_expressions,
      bool requires_input_row);
  PhysicalExpression(const PhysicalExpression &) = delete;
  PhysicalExpression &operator=(const PhysicalExpression &) = delete;
  PhysicalExpression(PhysicalExpression &&) noexcept = default;
  PhysicalExpression &operator=(PhysicalExpression &&) noexcept = default;

  [[nodiscard]] const ast::Expression *Expression() const noexcept {
    return expression_.get();
  }
  [[nodiscard]] const std::vector<ir::LogicalPrecomputedExpression> &
  PrecomputedExpressions() const noexcept {
    return precomputed_expression_views_;
  }
  [[nodiscard]] bool RequiresInputRow() const noexcept {
    return requires_input_row_;
  }

 private:
  std::unique_ptr<ast::Expression> expression_;
  std::vector<PhysicalPrecomputedExpression> precomputed_expressions_;
  std::vector<ir::LogicalPrecomputedExpression> precomputed_expression_views_;
  bool requires_input_row_ = false;
};

struct AllNodeScanOp {
  std::string variable;
};

struct FilterOp {
  PhysicalExpression predicate;
};

struct PhysicalProjectionItem {
  std::string alias;
  PhysicalExpression expression;
  bool passthrough = false;
};

struct ProjectionOp {
  std::vector<PhysicalProjectionItem> items;
};

struct SkipOp {
  PhysicalExpression count;
};

struct LimitOp {
  PhysicalExpression count;
  bool exhaust_child = false;
};

struct ProduceResultsOp {
  std::vector<std::string> columns;
};

using PhysicalOperatorData =
    std::variant<std::monostate, AllNodeScanOp, FilterOp, ProjectionOp, SkipOp,
                 LimitOp, ProduceResultsOp>;

struct PhysicalPlanNode {
  OperatorId id = 0;
  PhysicalOperatorKind kind = PhysicalOperatorKind::kArgument;
  PhysicalOperatorData data;
  // Transitional debug origin. Runtime operators for migrated physical kinds
  // must not read this pointer.
  const ir::LogicalPlan *logical = nullptr;
  const ir::TopNPlan *top_n = nullptr;
  std::string logical_name;
  std::string details;
  std::optional<double> estimated_rows;
  std::optional<double> cost;
  SlotConfigurationPtr argument_slots;
  SlotConfigurationPtr output_slots;
  std::vector<ir::LogicalSortItem> provided_order;
  std::vector<SlotMapping> argument_mapping;
  std::vector<std::vector<SlotMapping>> child_mappings;
  std::vector<std::unique_ptr<PhysicalPlanNode>> children;
  std::size_t partial_sort_prefix = 0;
  std::size_t partial_top_n_prefix = 0;
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
