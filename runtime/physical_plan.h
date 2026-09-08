#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ast/ast_node.h"
#include "ast/precomputed_expression.h"
#include "runtime/slotted_row.h"

namespace ir {
class LogicalPlan;
}

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
  kUnionAll,
  kUnionDistinct,
};

[[nodiscard]] std::string_view ToString(PhysicalOperatorKind kind);

enum class PhysicalExpandDirection {
  kIncoming,
  kOutgoing,
  kBoth,
};

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
  [[nodiscard]] const std::vector<ast::PrecomputedExpression> &
  PrecomputedExpressions() const noexcept {
    return precomputed_expression_views_;
  }
  [[nodiscard]] bool RequiresInputRow() const noexcept {
    return requires_input_row_;
  }

 private:
  std::unique_ptr<ast::Expression> expression_;
  std::vector<PhysicalPrecomputedExpression> precomputed_expressions_;
  std::vector<ast::PrecomputedExpression> precomputed_expression_views_;
  bool requires_input_row_ = false;
};

struct AllNodeScanOp {
  std::string variable;
};

struct ArgumentOp {};

struct NodeByLabelScanOp {
  std::string variable;
  std::vector<std::string> labels;
};

struct NodeIndexSeekOp {
  std::string variable;
  std::vector<std::string> labels;
  std::string property_key;
  PhysicalExpression value;
  bool unique = false;
};

struct NodeIndexRangeSeekOp {
  std::string variable;
  std::vector<std::string> labels;
  std::string property_key;
  std::vector<PhysicalExpression> predicates;
};

struct PhysicalRelationshipPattern {
  std::string from_node;
  std::string relationship;
  std::string to_node;
  PhysicalExpandDirection direction = PhysicalExpandDirection::kBoth;
  std::vector<std::string> types;
};

struct PhysicalRelationshipLength {
  bool variable = false;
  std::optional<int> min;
  std::optional<int> max;
};

struct RelationshipTypeScanOp {
  PhysicalRelationshipPattern pattern;
};

struct RelationshipIndexSeekOp {
  PhysicalRelationshipPattern pattern;
  std::string property_key;
  PhysicalExpression value;
  bool unique = false;
};

struct RelationshipIndexRangeSeekOp {
  PhysicalRelationshipPattern pattern;
  std::string property_key;
  std::vector<PhysicalExpression> predicates;
};

struct NodeByIdSeekOp {
  std::string variable;
  PhysicalExpression ids;
  bool many = false;
};

struct RelationshipByIdSeekOp {
  PhysicalRelationshipPattern pattern;
  PhysicalExpression ids;
  bool many = false;
};

struct ExpandOp {
  PhysicalRelationshipPattern pattern;
  Slot from_node_input_slot;
  Slot relationship_output_slot;
  Slot to_node_output_slot;
};

struct ExpandIntoOp {
  PhysicalRelationshipPattern pattern;
  Slot from_node_input_slot;
  Slot to_node_input_slot;
  Slot relationship_output_slot;
};

struct VarExpandOp {
  PhysicalRelationshipPattern pattern;
  PhysicalRelationshipLength length;
  Slot from_node_input_slot;
  std::optional<Slot> to_node_input_slot;
  Slot relationship_output_slot;
  Slot to_node_output_slot;
};

struct PruningVarExpandOp {
  PhysicalRelationshipPattern pattern;
  PhysicalRelationshipLength length;
  Slot from_node_input_slot;
  Slot to_node_output_slot;
};

struct OptionalExpandOp {
  PhysicalRelationshipPattern pattern;
  std::vector<PhysicalExpression> predicates;
  Slot from_node_input_slot;
  Slot relationship_output_slot;
  Slot to_node_output_slot;
  std::vector<Slot> output_slots;
};

struct PhysicalPathPattern {
  std::string variable;
  std::vector<std::string> nodes;
  std::vector<std::string> relationships;
};

struct PathBuildOp {
  PhysicalPathPattern path;
  std::vector<Slot> node_input_slots;
  std::vector<Slot> relationship_input_slots;
  Slot path_output_slot;
};

struct ProjectEndpointsOp {
  PhysicalRelationshipPattern pattern;
  PhysicalRelationshipLength length;
  Slot relationship_input_slot;
  std::optional<Slot> from_node_input_slot;
  std::optional<Slot> to_node_input_slot;
  Slot from_node_output_slot;
  Slot to_node_output_slot;
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

struct PhysicalGroupingItem {
  std::string alias;
  PhysicalExpression expression;
};

struct HashDistinctOp {
  std::vector<PhysicalGroupingItem> grouping_items;
};

struct OrderedDistinctOp {
  std::vector<PhysicalGroupingItem> grouping_items;
};

struct PhysicalAggregationItem {
  std::string alias;
  PhysicalExpression expression;
};

struct HashAggregationOp {
  std::vector<PhysicalGroupingItem> grouping_items;
  std::vector<PhysicalAggregationItem> aggregation_items;
};

struct OrderedAggregationOp {
  std::vector<PhysicalGroupingItem> grouping_items;
  std::vector<PhysicalAggregationItem> aggregation_items;
};

enum class PhysicalSortDirection {
  kAscending,
  kDescending,
};

[[nodiscard]] std::string_view ToString(PhysicalSortDirection direction);

struct PhysicalOrderingItem {
  std::shared_ptr<const ast::Expression> expression;
  PhysicalSortDirection direction = PhysicalSortDirection::kAscending;
};

using PhysicalOrdering = std::vector<PhysicalOrderingItem>;

struct PhysicalOperatorTraits {
  bool writes = false;
  bool write_barrier = false;
};

struct PhysicalPlanEffects {
  bool writes = false;
  bool contains_write_barrier = false;
};

struct PhysicalSortItem {
  PhysicalExpression expression;
  PhysicalSortDirection direction = PhysicalSortDirection::kAscending;
};

struct FullSortOp {
  std::vector<PhysicalSortItem> items;
};

struct PartialSortOp {
  std::vector<PhysicalSortItem> items;
  std::size_t prefix = 0;
};

struct TopNOp {
  std::vector<PhysicalSortItem> items;
  PhysicalExpression limit;
};

struct PartialTopNOp {
  std::vector<PhysicalSortItem> items;
  PhysicalExpression limit;
  std::size_t prefix = 0;
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

struct PhysicalAssertedNode {
  std::string variable;
  Slot input_slot;
};

struct AssertIsNodeOp {
  std::vector<PhysicalAssertedNode> nodes;
};

struct UnwindOp {
  PhysicalExpression expression;
  Slot value_slot;
};

struct PhysicalProcedureYield {
  std::string result_field;
  Slot output_slot;
};

struct ProcedureCallOp {
  std::string procedure_name;
  std::vector<PhysicalExpression> arguments;
  std::vector<PhysicalProcedureYield> yields;
  bool read_only = false;
};

struct PhysicalValueHashJoinKey {
  PhysicalExpression left;
  PhysicalExpression right;
};

struct ValueHashJoinOp {
  std::vector<PhysicalValueHashJoinKey> keys;
  std::vector<PhysicalExpression> predicates;
  std::size_t build_child = 1;
};

struct NodeHashJoinOp {
  std::vector<std::string> join_keys;
  std::size_t build_child = 1;
};

struct LeftOuterHashJoinOp {
  std::vector<std::string> join_keys;
};

struct CartesianProductOp {
  std::size_t cached_child = 1;
};

struct PredicateJoinOp {
  std::vector<PhysicalExpression> predicates;
  std::size_t cached_child = 1;
};

struct UnionAllOp {};

struct UnionDistinctOp {
  std::vector<Slot> key_slots;
};

struct ApplyOp {};

struct OptionalApplyOp {};

struct SemiApplyOp {};

struct AntiSemiApplyOp {};

struct LetSemiApplyOp {
  Slot value_slot;
};

struct SelectOrSemiApplyOp {
  PhysicalExpression predicate;
  bool anti = false;
};

struct RollUpApplyOp {
  Slot collection_slot;
  Slot value_slot;
};

struct PhysicalPropertyMapEntry {
  std::string key;
  PhysicalExpression value;
};

struct PhysicalPropertyMap {
  std::vector<PhysicalPropertyMapEntry> entries;
  std::optional<PhysicalExpression> parameter;
};

struct WriteBarrierOp {};

struct CreateNodeOp {
  Slot node_slot;
  std::vector<std::string> labels;
  PhysicalPropertyMap properties;
};

struct CreateRelationshipOp {
  Slot relationship_slot;
  Slot left_node_slot;
  Slot right_node_slot;
  std::string type;
  PhysicalPropertyMap properties;
};

struct SetPropertyOp {
  PhysicalExpression entity;
  std::string property_key;
  PhysicalExpression value;
};

struct SetPropertiesOp {
  PhysicalExpression entity;
  PhysicalExpression value;
  bool include_existing = false;
};

struct SetLabelsOp {
  PhysicalExpression entity;
  std::vector<std::string> labels;
};

using PhysicalMergeCreateCommand =
    std::variant<CreateNodeOp, CreateRelationshipOp>;
using PhysicalMergeSetOperation =
    std::variant<SetPropertyOp, SetPropertiesOp, SetLabelsOp>;

struct PhysicalMergeAction {
  bool on_match = false;
  std::vector<PhysicalMergeSetOperation> set_operations;
};

struct MergeOp {
  std::vector<PhysicalMergeCreateCommand> create_commands;
  std::vector<PhysicalMergeAction> actions;
};

struct RemovePropertyOp {
  PhysicalExpression entity;
  std::string property_key;
};

struct RemoveLabelsOp {
  PhysicalExpression entity;
  std::vector<std::string> labels;
};

struct DeleteOp {
  static constexpr bool kDetach = false;
  std::vector<PhysicalExpression> expressions;
};

struct DetachDeleteOp {
  static constexpr bool kDetach = true;
  std::vector<PhysicalExpression> expressions;
};

using PhysicalOperatorData = std::variant<
    std::monostate, ArgumentOp, AllNodeScanOp, NodeByLabelScanOp,
    NodeIndexSeekOp, NodeIndexRangeSeekOp, RelationshipTypeScanOp,
    RelationshipIndexSeekOp, RelationshipIndexRangeSeekOp, NodeByIdSeekOp,
    RelationshipByIdSeekOp, ExpandOp, ExpandIntoOp, VarExpandOp,
    PruningVarExpandOp, OptionalExpandOp, ProjectEndpointsOp, PathBuildOp,
    FilterOp, ProjectionOp, HashDistinctOp, OrderedDistinctOp,
    HashAggregationOp, OrderedAggregationOp, FullSortOp, PartialSortOp, TopNOp,
    PartialTopNOp, SkipOp, LimitOp, ProduceResultsOp, AssertIsNodeOp, UnwindOp,
    ProcedureCallOp, ValueHashJoinOp, NodeHashJoinOp, LeftOuterHashJoinOp,
    CartesianProductOp, PredicateJoinOp, UnionAllOp, UnionDistinctOp, ApplyOp,
    OptionalApplyOp, SemiApplyOp, AntiSemiApplyOp, LetSemiApplyOp,
    SelectOrSemiApplyOp, RollUpApplyOp, WriteBarrierOp, CreateNodeOp,
    CreateRelationshipOp, MergeOp, SetPropertyOp, SetPropertiesOp, SetLabelsOp,
    RemovePropertyOp, RemoveLabelsOp, DeleteOp, DetachDeleteOp>;

struct PhysicalPlanNode {
  OperatorId id = 0;
  PhysicalOperatorKind kind = PhysicalOperatorKind::kArgument;
  PhysicalOperatorData data;
  std::string logical_name;
  std::string details;
  std::optional<double> estimated_rows;
  std::optional<double> cost;
  SlotConfigurationPtr argument_slots;
  SlotConfigurationPtr output_slots;
  PhysicalOrdering provided_order;
  PhysicalOperatorTraits traits;
  PhysicalPlanEffects subtree_effects;
  std::vector<SlotMapping> argument_mapping;
  std::vector<std::vector<SlotMapping>> child_mappings;
  std::vector<std::unique_ptr<PhysicalPlanNode>> children;
};

class PhysicalPlan final {
 public:
  explicit PhysicalPlan(std::unique_ptr<PhysicalPlanNode> root);

  [[nodiscard]] const PhysicalPlanNode &Root() const;
  [[nodiscard]] const PhysicalPlanEffects &Effects() const noexcept;

 private:
  std::unique_ptr<PhysicalPlanNode> root_;
};

[[nodiscard]] PhysicalPlan CreatePhysicalPlan(const ir::LogicalPlan &plan);

}  // namespace rg
