#include "runtime/physical_plan.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "ast/ast_clone.h"
#include "ast/ast_equal.h"
#include "ast/ast_node.h"
#include "ast/expression_dependency.h"
#include "common/exception.h"
#include "ir/logical_plan.h"

namespace rg {

PhysicalExpression::PhysicalExpression(
    std::unique_ptr<ast::Expression> expression,
    std::vector<PhysicalPrecomputedExpression> precomputed_expressions,
    bool requires_input_row)
    : expression_(std::move(expression)),
      precomputed_expressions_(std::move(precomputed_expressions)),
      requires_input_row_(requires_input_row) {
  precomputed_expression_views_.reserve(precomputed_expressions_.size());
  for (const auto &precomputed : precomputed_expressions_) {
    precomputed_expression_views_.push_back(
        {.expression = precomputed.expression.get(),
         .variable = precomputed.variable});
  }
}

namespace {

PhysicalSortDirection ToPhysicalSortDirection(
    ir::LogicalOrderDirection direction) {
  switch (direction) {
    case ir::LogicalOrderDirection::kAscending:
      return PhysicalSortDirection::kAscending;
    case ir::LogicalOrderDirection::kDescending:
      return PhysicalSortDirection::kDescending;
  }
  RG_THROW(common::ErrorCode::InternalError, "unknown sort direction");
}

PhysicalOrdering CopyPhysicalOrdering(
    const std::vector<ir::LogicalSortItem> &items) {
  PhysicalOrdering copied;
  copied.reserve(items.size());
  for (const auto &item : items) {
    RG_CHECK(item.expression != nullptr, common::ErrorCode::InvalidParameter,
             "physical ordering expression is null");
    copied.push_back({.expression = std::shared_ptr<const ast::Expression>(
                          ast::CloneExpression(*item.expression)),
                      .direction = ToPhysicalSortDirection(item.direction)});
  }
  return copied;
}

PhysicalOperatorTraits OperatorTraits(const PhysicalPlanNode &node) {
  switch (node.kind) {
    case PhysicalOperatorKind::kProcedureCall:
      return {.writes = !std::get<ProcedureCallOp>(node.data).read_only};
    case PhysicalOperatorKind::kWriteBarrier:
      return {.write_barrier = true};
    case PhysicalOperatorKind::kCreateNode:
    case PhysicalOperatorKind::kCreateRelationship:
    case PhysicalOperatorKind::kMerge:
    case PhysicalOperatorKind::kSetProperty:
    case PhysicalOperatorKind::kSetProperties:
    case PhysicalOperatorKind::kSetLabels:
    case PhysicalOperatorKind::kRemoveProperty:
    case PhysicalOperatorKind::kRemoveLabels:
    case PhysicalOperatorKind::kDelete:
    case PhysicalOperatorKind::kDetachDelete:
      return {.writes = true};
    default:
      return {};
  }
}

void BuildEffects(PhysicalPlanNode *node) {
  RG_CHECK(node != nullptr, common::ErrorCode::InternalError,
           "physical plan node is null");
  node->traits = OperatorTraits(*node);
  node->subtree_effects = {
      .writes = node->traits.writes,
      .contains_write_barrier = node->traits.write_barrier};
  for (const auto &child : node->children) {
    node->subtree_effects.writes |= child->subtree_effects.writes;
    node->subtree_effects.contains_write_barrier |=
        child->subtree_effects.contains_write_barrier;
  }
}

std::vector<SlotMapping> ComputeSlotMappings(const SlotConfiguration &source,
                                             const SlotConfiguration &target) {
  std::vector<SlotMapping> mappings;
  for (const auto &column : target.Columns()) {
    const std::optional<std::size_t> source_offset = source.Find(column);
    if (source_offset.has_value()) {
      mappings.push_back({.source_offset = *source_offset,
                          .target_offset = target.At(column)});
    }
  }
  return mappings;
}

bool SameSlotLayout(const SlotConfiguration &left,
                    const SlotConfiguration &right) {
  return left.Columns() == right.Columns() &&
         left.SlotCount() == right.SlotCount();
}

std::size_t CommonOrderingPrefix(const PhysicalOrdering &provided,
                                 const PhysicalOrdering &required) {
  const std::size_t limit = std::min(provided.size(), required.size());
  std::size_t prefix = 0;
  while (prefix < limit && provided[prefix].expression != nullptr &&
         required[prefix].expression != nullptr &&
         provided[prefix].direction == required[prefix].direction &&
         ast::ASTEqual::Equal(provided[prefix].expression.get(),
                              required[prefix].expression.get())) {
    ++prefix;
  }
  return prefix;
}

PhysicalOrderingItem AliasOrderingItem(std::string alias,
                                       PhysicalSortDirection direction) {
  auto variable = std::make_shared<ast::Variable>();
  variable->name = std::move(alias);
  return {.expression = std::move(variable), .direction = direction};
}

std::optional<PhysicalOrdering> OrderedGroupingOutput(
    const PhysicalOrdering &input_order,
    const std::vector<ir::LogicalProjectionItem> &grouping_items) {
  if (grouping_items.empty() || input_order.size() < grouping_items.size()) {
    return std::nullopt;
  }

  std::vector<bool> matched(grouping_items.size(), false);
  PhysicalOrdering output_order;
  output_order.reserve(grouping_items.size());
  for (std::size_t order_index = 0; order_index < grouping_items.size();
       ++order_index) {
    const PhysicalOrderingItem &order = input_order[order_index];
    if (order.expression == nullptr) {
      return std::nullopt;
    }
    std::optional<std::size_t> grouping_index;
    for (std::size_t index = 0; index < grouping_items.size(); ++index) {
      const ir::LogicalProjectionItem &grouping = grouping_items[index];
      if (!matched[index] && grouping.expression != nullptr &&
          ast::ASTEqual::Equal(order.expression.get(), grouping.expression)) {
        grouping_index = index;
        break;
      }
    }
    if (!grouping_index.has_value()) {
      return std::nullopt;
    }
    matched[*grouping_index] = true;
    output_order.push_back(AliasOrderingItem(
        grouping_items[*grouping_index].alias, order.direction));
  }
  return output_order;
}

PhysicalOrdering ProjectOrdering(const PhysicalOrdering &ordering,
                                 const ir::ProjectionPlan &projection) {
  PhysicalOrdering projected;
  projected.reserve(ordering.size());
  const std::unordered_set<std::string> output_symbols(
      projection.OutputColumns().begin(), projection.OutputColumns().end());

  for (const auto &order_item : ordering) {
    if (order_item.expression == nullptr) {
      break;
    }
    const auto matching_item = std::find_if(
        projection.Items().begin(), projection.Items().end(),
        [&](const ir::LogicalProjectionItem &item) {
          return item.expression != nullptr && !item.alias.empty() &&
                 ast::ASTEqual::Equal(item.expression,
                                      order_item.expression.get());
        });
    if (matching_item != projection.Items().end()) {
      projected.push_back(
          AliasOrderingItem(matching_item->alias, order_item.direction));
      continue;
    }
    const auto dependencies =
        ast::CollectExpressionDependencies(*order_item.expression);
    if (std::all_of(dependencies.begin(), dependencies.end(),
                    [&](const std::string &dependency) {
                      return output_symbols.contains(dependency);
                    })) {
      projected.push_back(order_item);
      continue;
    }
    break;
  }
  return projected;
}

std::vector<std::string> AppendUnique(std::vector<std::string> columns,
                                      const std::vector<std::string> &added) {
  std::unordered_set<std::string> seen(columns.begin(), columns.end());
  for (const auto &column : added) {
    if (!column.empty() && seen.insert(column).second) {
      columns.push_back(column);
    }
  }
  return columns;
}

SlotConfigurationPtr MakeLayout(std::vector<std::string> columns) {
  return std::make_shared<const SlotConfiguration>(std::move(columns));
}

std::vector<SlotMapping> NamedMappings(
    const SlotConfiguration &source, const SlotConfiguration &target,
    const std::vector<std::pair<std::string, std::string>> &names) {
  std::vector<SlotMapping> mappings;
  mappings.reserve(names.size());
  for (const auto &[source_name, target_name] : names) {
    const std::optional<std::size_t> source_offset = source.Find(source_name);
    const std::optional<std::size_t> target_offset = target.Find(target_name);
    if (source_offset.has_value() && target_offset.has_value()) {
      mappings.push_back(
          {.source_offset = *source_offset, .target_offset = *target_offset});
    }
  }
  return mappings;
}

std::optional<std::size_t> FindSlot(const SlotConfiguration &slots,
                                    std::string_view name) {
  return slots.Find(name);
}

std::optional<std::size_t> OptionalOutputSlot(const SlotConfiguration &slots,
                                              std::string_view name) {
  if (name.empty()) {
    return std::nullopt;
  }
  return slots.At(name);
}

PhysicalRelationshipSlots ResolveRelationshipOutputSlots(
    const PhysicalRelationshipPattern &pattern,
    const SlotConfiguration &output_slots) {
  return {
      .from_node_output_slot =
          OptionalOutputSlot(output_slots, pattern.from_node),
      .relationship_output_slot =
          OptionalOutputSlot(output_slots, pattern.relationship),
      .to_node_output_slot = OptionalOutputSlot(output_slots, pattern.to_node),
  };
}

std::vector<std::size_t> ResolveSlots(const SlotConfiguration &slots,
                                      const std::vector<std::string> &names) {
  std::vector<std::size_t> offsets;
  offsets.reserve(names.size());
  for (const auto &name : names) {
    offsets.push_back(slots.At(name));
  }
  return offsets;
}

std::vector<std::size_t> AllSlots(const SlotConfiguration &slots) {
  std::vector<std::size_t> offsets;
  offsets.reserve(slots.SlotCount());
  for (std::size_t offset = 0; offset < slots.SlotCount(); ++offset) {
    offsets.push_back(offset);
  }
  return offsets;
}

PhysicalExpression CopyPhysicalExpression(
    const ast::Expression *expression,
    const std::vector<ast::PrecomputedExpression> &precomputed) {
  RG_CHECK(expression != nullptr, common::ErrorCode::InvalidParameter,
           "physical expression is null");
  std::vector<PhysicalPrecomputedExpression> copied_precomputed;
  copied_precomputed.reserve(precomputed.size());
  for (const auto &entry : precomputed) {
    RG_CHECK(entry.expression != nullptr, common::ErrorCode::InvalidParameter,
             "physical precomputed expression is null");
    copied_precomputed.push_back(
        {.expression = ast::CloneExpression(*entry.expression),
         .variable = entry.variable});
  }
  const bool requires_input_row =
      !ast::CollectExpressionDependencies(*expression).empty() ||
      !precomputed.empty();
  return PhysicalExpression(ast::CloneExpression(*expression),
                            std::move(copied_precomputed), requires_input_row);
}

std::vector<PhysicalExpression> CopyPhysicalExpressions(
    const std::vector<const ast::Expression *> &expressions) {
  std::vector<PhysicalExpression> copied;
  copied.reserve(expressions.size());
  for (const ast::Expression *expression : expressions) {
    copied.push_back(CopyPhysicalExpression(expression, {}));
  }
  return copied;
}

PhysicalPropertyMap CopyPhysicalPropertyMap(
    const ir::PatternPropertyMap &properties) {
  PhysicalPropertyMap copied;
  copied.entries.reserve(properties.entries.size());
  for (const auto &entry : properties.entries) {
    copied.entries.push_back(
        {.key = entry.key, .value = CopyPhysicalExpression(entry.value, {})});
  }
  if (properties.parameter != nullptr) {
    copied.parameter.emplace(CopyPhysicalExpression(properties.parameter, {}));
  }
  return copied;
}

PhysicalMergeSetOperation CopyPhysicalMergeSetOperation(
    const ir::SetMutatingPattern &pattern) {
  switch (pattern.kind) {
    case ir::SetMutatingPatternKind::kSetProperty:
      return SetPropertyOp{.entity = CopyPhysicalExpression(pattern.entity, {}),
                           .property_key = pattern.property_key,
                           .value = CopyPhysicalExpression(pattern.value, {})};
    case ir::SetMutatingPatternKind::kSetExactPropertiesFromMap:
      return SetPropertiesOp{
          .entity = CopyPhysicalExpression(pattern.entity, {}),
          .value = CopyPhysicalExpression(pattern.value, {}),
          .include_existing = false};
    case ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap:
      return SetPropertiesOp{
          .entity = CopyPhysicalExpression(pattern.entity, {}),
          .value = CopyPhysicalExpression(pattern.value, {}),
          .include_existing = true};
    case ir::SetMutatingPatternKind::kSetLabels:
      return SetLabelsOp{.entity = CopyPhysicalExpression(pattern.entity, {}),
                         .labels = pattern.labels};
  }
  RG_THROW(common::ErrorCode::InternalError, "unknown MERGE SET operation");
}

MergeOp CopyPhysicalMerge(const ir::MergePattern &merge,
                          const SlotConfiguration &output_slots) {
  MergeOp copied;
  copied.create_commands.reserve(merge.create_pattern.commands.size());
  for (const auto &command : merge.create_pattern.commands) {
    switch (command.kind) {
      case ir::CreateEntityKind::kNode: {
        RG_CHECK(command.index < merge.create_pattern.nodes.size(),
                 common::ErrorCode::InvalidParameter,
                 "MERGE node command index is out of range");
        const ir::CreateNodePattern &node =
            merge.create_pattern.nodes[command.index];
        copied.create_commands.emplace_back(CreateNodeOp{
            .node_slot = output_slots.At(node.variable),
            .labels = node.labels,
            .properties = CopyPhysicalPropertyMap(node.properties)});
        break;
      }
      case ir::CreateEntityKind::kRelationship: {
        RG_CHECK(command.index < merge.create_pattern.relationships.size(),
                 common::ErrorCode::InvalidParameter,
                 "MERGE relationship command index is out of range");
        const ir::CreateRelationshipPattern &relationship =
            merge.create_pattern.relationships[command.index];
        const bool incoming =
            relationship.direction == ir::Direction::kIncoming;
        copied.create_commands.emplace_back(CreateRelationshipOp{
            .relationship_slot = output_slots.At(relationship.variable),
            .left_node_slot = output_slots.At(
                incoming ? relationship.right_node : relationship.left_node),
            .right_node_slot = output_slots.At(
                incoming ? relationship.left_node : relationship.right_node),
            .type = relationship.types.empty() ? std::string()
                                               : relationship.types.front(),
            .properties = CopyPhysicalPropertyMap(relationship.properties)});
        break;
      }
    }
  }

  copied.actions.reserve(merge.actions.size());
  for (const auto &action : merge.actions) {
    PhysicalMergeAction physical_action{.on_match = action.on_match};
    physical_action.set_operations.reserve(action.set_patterns.size());
    for (const auto &set_pattern : action.set_patterns) {
      physical_action.set_operations.push_back(
          CopyPhysicalMergeSetOperation(set_pattern));
    }
    copied.actions.push_back(std::move(physical_action));
  }
  return copied;
}

PhysicalExpandDirection ToPhysicalExpandDirection(ir::Direction direction) {
  switch (direction) {
    case ir::Direction::kIncoming:
      return PhysicalExpandDirection::kIncoming;
    case ir::Direction::kOutgoing:
      return PhysicalExpandDirection::kOutgoing;
    case ir::Direction::kBoth:
      return PhysicalExpandDirection::kBoth;
  }
  RG_THROW(common::ErrorCode::InternalError, "unknown relationship direction");
}

PhysicalExpandDirection ToPhysicalExpandDirection(
    ir::ExpandDirection direction) {
  switch (direction) {
    case ir::ExpandDirection::kIncoming:
      return PhysicalExpandDirection::kIncoming;
    case ir::ExpandDirection::kOutgoing:
      return PhysicalExpandDirection::kOutgoing;
    case ir::ExpandDirection::kBoth:
      return PhysicalExpandDirection::kBoth;
  }
  RG_THROW(common::ErrorCode::InternalError, "unknown expand direction");
}

std::vector<PhysicalSortItem> CopyPhysicalSortItems(
    const std::vector<ir::LogicalSortItem> &items) {
  std::vector<PhysicalSortItem> copied;
  copied.reserve(items.size());
  for (const auto &item : items) {
    copied.push_back({.expression = CopyPhysicalExpression(
                          item.expression, item.precomputed_expressions),
                      .direction = ToPhysicalSortDirection(item.direction)});
  }
  return copied;
}

std::vector<PhysicalGroupingItem> CopyPhysicalGroupingItems(
    const std::vector<ir::LogicalProjectionItem> &items,
    const SlotConfiguration &output_slots) {
  std::vector<PhysicalGroupingItem> copied;
  copied.reserve(items.size());
  for (const auto &item : items) {
    copied.push_back({.alias = item.alias,
                      .output_slot = output_slots.At(item.alias),
                      .expression = CopyPhysicalExpression(
                          item.expression, item.precomputed_expressions)});
  }
  return copied;
}

std::vector<PhysicalAggregationItem> CopyPhysicalAggregationItems(
    const std::vector<ir::LogicalProjectionItem> &items,
    const SlotConfiguration &output_slots) {
  std::vector<PhysicalAggregationItem> copied;
  copied.reserve(items.size());
  for (const auto &item : items) {
    copied.push_back({.alias = item.alias,
                      .output_slot = output_slots.At(item.alias),
                      .expression = CopyPhysicalExpression(
                          item.expression, item.precomputed_expressions)});
  }
  return copied;
}

PhysicalRelationshipPattern CopyPhysicalRelationshipPattern(
    const ir::PatternRelationship &pattern) {
  return {.from_node = pattern.left_node,
          .relationship = pattern.variable,
          .to_node = pattern.right_node,
          .direction = ToPhysicalExpandDirection(pattern.direction),
          .types = pattern.types};
}

class PhysicalPlanBuilder final {
 public:
  PhysicalPlan Build(const ir::LogicalPlan &plan) {
    empty_slots_ = MakeLayout({});
    std::unique_ptr<PhysicalPlanNode> root = BuildNode(plan, empty_slots_);
    std::vector<std::string> result_columns;
    if (plan.Type() == ir::LogicalPlanNodeType::kProduceResults ||
        !root->subtree_effects.writes) {
      result_columns = plan.OutputColumns();
    }
    return PhysicalPlan(std::move(root), std::move(result_columns));
  }

 private:
  PhysicalOperatorKind SelectOperatorKind(const ir::LogicalPlan &plan) {
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kArgument:
        return PhysicalOperatorKind::kArgument;
      case ir::LogicalPlanNodeType::kAllNodeScan:
        return PhysicalOperatorKind::kAllNodeScan;
      case ir::LogicalPlanNodeType::kNodeByLabelScan:
        return PhysicalOperatorKind::kNodeByLabelScan;
      case ir::LogicalPlanNodeType::kNodeIndexSeek:
        return PhysicalOperatorKind::kNodeIndexSeek;
      case ir::LogicalPlanNodeType::kNodeIndexRangeSeek:
        return PhysicalOperatorKind::kNodeIndexRangeSeek;
      case ir::LogicalPlanNodeType::kRelationshipTypeScan:
        return PhysicalOperatorKind::kRelationshipTypeScan;
      case ir::LogicalPlanNodeType::kRelationshipIndexSeek:
        return PhysicalOperatorKind::kRelationshipIndexSeek;
      case ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek:
        return PhysicalOperatorKind::kRelationshipIndexRangeSeek;
      case ir::LogicalPlanNodeType::kExpand:
        return PhysicalOperatorKind::kExpand;
      case ir::LogicalPlanNodeType::kExpandInto:
        return PhysicalOperatorKind::kExpandInto;
      case ir::LogicalPlanNodeType::kVarExpand:
        return PhysicalOperatorKind::kVarExpand;
      case ir::LogicalPlanNodeType::kPathBuild:
        return PhysicalOperatorKind::kPathBuild;
      case ir::LogicalPlanNodeType::kFilter:
        return PhysicalOperatorKind::kFilter;
      case ir::LogicalPlanNodeType::kProjection:
        return PhysicalOperatorKind::kProjection;
      case ir::LogicalPlanNodeType::kDistinct:
        return PhysicalOperatorKind::kHashDistinct;
      case ir::LogicalPlanNodeType::kAggregation:
        return PhysicalOperatorKind::kHashAggregation;
      case ir::LogicalPlanNodeType::kSort:
        return PhysicalOperatorKind::kFullSort;
      case ir::LogicalPlanNodeType::kSkip:
        return PhysicalOperatorKind::kSkip;
      case ir::LogicalPlanNodeType::kLimit:
        return PhysicalOperatorKind::kLimit;
      case ir::LogicalPlanNodeType::kTopN:
        return PhysicalOperatorKind::kTopN;
      case ir::LogicalPlanNodeType::kProduceResults:
        return PhysicalOperatorKind::kProduceResults;
      case ir::LogicalPlanNodeType::kCartesianProduct:
        return PhysicalOperatorKind::kCartesianProduct;
      case ir::LogicalPlanNodeType::kNodeHashJoin:
        return PhysicalOperatorKind::kNodeHashJoin;
      case ir::LogicalPlanNodeType::kValueHashJoin:
        return PhysicalOperatorKind::kValueHashJoin;
      case ir::LogicalPlanNodeType::kPredicateJoin:
        return PhysicalOperatorKind::kPredicateJoin;
      case ir::LogicalPlanNodeType::kApply:
        return PhysicalOperatorKind::kApply;
      case ir::LogicalPlanNodeType::kSemiApply:
        return PhysicalOperatorKind::kSemiApply;
      case ir::LogicalPlanNodeType::kAntiSemiApply:
        return PhysicalOperatorKind::kAntiSemiApply;
      case ir::LogicalPlanNodeType::kLetSemiApply:
        return PhysicalOperatorKind::kLetSemiApply;
      case ir::LogicalPlanNodeType::kRollUpApply:
        return PhysicalOperatorKind::kRollUpApply;
      case ir::LogicalPlanNodeType::kOptionalApply:
        return PhysicalOperatorKind::kOptionalApply;
      case ir::LogicalPlanNodeType::kAssertIsNode:
        return PhysicalOperatorKind::kAssertIsNode;
      case ir::LogicalPlanNodeType::kWriteBarrier:
        return PhysicalOperatorKind::kWriteBarrier;
      case ir::LogicalPlanNodeType::kCreateNode:
        return PhysicalOperatorKind::kCreateNode;
      case ir::LogicalPlanNodeType::kCreateRelationship:
        return PhysicalOperatorKind::kCreateRelationship;
      case ir::LogicalPlanNodeType::kMerge:
        return PhysicalOperatorKind::kMerge;
      case ir::LogicalPlanNodeType::kSetProperty:
        return PhysicalOperatorKind::kSetProperty;
      case ir::LogicalPlanNodeType::kSetProperties:
        return PhysicalOperatorKind::kSetProperties;
      case ir::LogicalPlanNodeType::kSetLabels:
        return PhysicalOperatorKind::kSetLabels;
      case ir::LogicalPlanNodeType::kRemoveProperty:
        return PhysicalOperatorKind::kRemoveProperty;
      case ir::LogicalPlanNodeType::kRemoveLabels:
        return PhysicalOperatorKind::kRemoveLabels;
      case ir::LogicalPlanNodeType::kDelete:
        return PhysicalOperatorKind::kDelete;
      case ir::LogicalPlanNodeType::kDetachDelete:
        return PhysicalOperatorKind::kDetachDelete;
      case ir::LogicalPlanNodeType::kUnwind:
        return PhysicalOperatorKind::kUnwind;
      case ir::LogicalPlanNodeType::kProcedureCall:
        return PhysicalOperatorKind::kProcedureCall;
      case ir::LogicalPlanNodeType::kNodeByIdSeek:
        return PhysicalOperatorKind::kNodeByIdSeek;
      case ir::LogicalPlanNodeType::kRelationshipByIdSeek:
        return PhysicalOperatorKind::kRelationshipByIdSeek;
      case ir::LogicalPlanNodeType::kProjectEndpoints:
        return PhysicalOperatorKind::kProjectEndpoints;
      case ir::LogicalPlanNodeType::kOptionalExpand:
        return PhysicalOperatorKind::kOptionalExpand;
      case ir::LogicalPlanNodeType::kLeftOuterHashJoin:
        return PhysicalOperatorKind::kLeftOuterHashJoin;
      case ir::LogicalPlanNodeType::kSelectOrSemiApply:
        return PhysicalOperatorKind::kSelectOrSemiApply;
      case ir::LogicalPlanNodeType::kPruningVarExpand:
        return PhysicalOperatorKind::kPruningVarExpand;
      case ir::LogicalPlanNodeType::kUnion:
        return static_cast<const ir::UnionPlan &>(plan).All()
                   ? PhysicalOperatorKind::kUnionAll
                   : PhysicalOperatorKind::kUnionDistinct;
    }
    RG_THROW(common::ErrorCode::InternalError,
             "unsupported physical operator: " + std::string(plan.Name()));
  }

  std::unique_ptr<PhysicalPlanNode> BuildNode(
      const ir::LogicalPlan &plan, SlotConfigurationPtr argument_slots) {
    auto node = std::make_unique<PhysicalPlanNode>();
    node->id = next_id_++;
    node->logical_name = std::string(plan.Name());
    node->details = plan.Details();
    node->estimated_rows = plan.EstimatedRows();
    node->cost = plan.Cost();
    node->argument_slots = std::move(argument_slots);

    if (plan.ChildCount() == 1) {
      node->children.push_back(BuildNode(plan.Child(0), node->argument_slots));
    } else if (plan.ChildCount() == 2) {
      node->children.push_back(BuildNode(plan.Child(0), node->argument_slots));
      const bool correlated =
          plan.Type() == ir::LogicalPlanNodeType::kApply ||
          plan.Type() == ir::LogicalPlanNodeType::kSemiApply ||
          plan.Type() == ir::LogicalPlanNodeType::kAntiSemiApply ||
          plan.Type() == ir::LogicalPlanNodeType::kLetSemiApply ||
          plan.Type() == ir::LogicalPlanNodeType::kSelectOrSemiApply ||
          plan.Type() == ir::LogicalPlanNodeType::kRollUpApply ||
          plan.Type() == ir::LogicalPlanNodeType::kOptionalApply ||
          plan.Type() == ir::LogicalPlanNodeType::kMerge;
      node->children.push_back(
          BuildNode(plan.Child(1), correlated ? node->children[0]->output_slots
                                              : node->argument_slots));
    }

    if (plan.Type() == ir::LogicalPlanNodeType::kUnion) {
      BuildUnionLayout(static_cast<const ir::UnionPlan &>(plan), node.get());
    } else {
      std::vector<std::string> columns = plan.OutputColumns();
      if (plan.ChildCount() == 0 &&
          plan.Type() != ir::LogicalPlanNodeType::kArgument) {
        columns = AppendUnique(node->argument_slots->Columns(), columns);
      }
      node->output_slots = MakeLayout(std::move(columns));
      // Slot configurations are immutable. Reuse an identical unary child
      // layout so compatible operators can forward rows without allocating a
      // second set of slot vectors.
      if (node->children.size() == 1 &&
          SameSlotLayout(*node->children.front()->output_slots,
                         *node->output_slots)) {
        node->output_slots = node->children.front()->output_slots;
      }
      for (const auto &child : node->children) {
        node->child_mappings.push_back(
            ComputeSlotMappings(*child->output_slots, *node->output_slots));
      }
    }
    node->argument_mapping =
        ComputeSlotMappings(*node->argument_slots, *node->output_slots);
    SelectOperator(plan, node.get());
    BuildOperatorData(plan, node.get());
    BuildEffects(node.get());
    return node;
  }

  void SelectOperator(const ir::LogicalPlan &plan, PhysicalPlanNode *node) {
    RG_CHECK(node != nullptr, common::ErrorCode::InternalError,
             "physical plan node is null");
    node->provided_order = CopyPhysicalOrdering(plan.OrderingTrait());
    if (!node->children.empty()) {
      switch (plan.Type()) {
        case ir::LogicalPlanNodeType::kFilter:
        case ir::LogicalPlanNodeType::kPathBuild:
        case ir::LogicalPlanNodeType::kSkip:
        case ir::LogicalPlanNodeType::kLimit:
        case ir::LogicalPlanNodeType::kProduceResults:
        case ir::LogicalPlanNodeType::kAssertIsNode:
        case ir::LogicalPlanNodeType::kWriteBarrier:
        case ir::LogicalPlanNodeType::kExpand:
        case ir::LogicalPlanNodeType::kExpandInto:
        case ir::LogicalPlanNodeType::kVarExpand:
        case ir::LogicalPlanNodeType::kSetProperty:
        case ir::LogicalPlanNodeType::kSetProperties:
        case ir::LogicalPlanNodeType::kSetLabels:
        case ir::LogicalPlanNodeType::kRemoveProperty:
        case ir::LogicalPlanNodeType::kRemoveLabels:
        case ir::LogicalPlanNodeType::kDelete:
        case ir::LogicalPlanNodeType::kDetachDelete:
        case ir::LogicalPlanNodeType::kApply:
        case ir::LogicalPlanNodeType::kOptionalApply:
        case ir::LogicalPlanNodeType::kOptionalExpand:
        case ir::LogicalPlanNodeType::kProjectEndpoints:
        case ir::LogicalPlanNodeType::kPruningVarExpand:
        case ir::LogicalPlanNodeType::kSelectOrSemiApply:
          node->provided_order = node->children[0]->provided_order;
          break;
        case ir::LogicalPlanNodeType::kProjection:
          node->provided_order =
              ProjectOrdering(node->children[0]->provided_order,
                              static_cast<const ir::ProjectionPlan &>(plan));
          break;
        default:
          break;
      }
    }
    if (plan.Type() == ir::LogicalPlanNodeType::kSort) {
      RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
               "Sort physical node must have one child");
      const auto &sort = static_cast<const ir::SortPlan &>(plan);
      PhysicalOrdering required_order = CopyPhysicalOrdering(sort.Items());
      const std::size_t prefix = CommonOrderingPrefix(
          node->children[0]->provided_order, required_order);
      node->kind = prefix == 0 ? PhysicalOperatorKind::kFullSort
                               : PhysicalOperatorKind::kPartialSort;
      node->provided_order = std::move(required_order);
      return;
    }

    if (plan.Type() == ir::LogicalPlanNodeType::kTopN) {
      RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
               "Top-N physical node must have one child");
      const auto &top_n = static_cast<const ir::TopNPlan &>(plan);
      PhysicalOrdering required_order = CopyPhysicalOrdering(top_n.Items());
      const std::size_t prefix = CommonOrderingPrefix(
          node->children[0]->provided_order, required_order);
      node->kind = prefix == 0 ? PhysicalOperatorKind::kTopN
                               : PhysicalOperatorKind::kPartialTopN;
      node->provided_order = std::move(required_order);
      return;
    }

    if (plan.Type() == ir::LogicalPlanNodeType::kDistinct) {
      RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
               "Distinct physical node must have one child");
      const auto &distinct = static_cast<const ir::DistinctPlan &>(plan);
      const auto output_order = OrderedGroupingOutput(
          node->children[0]->provided_order, distinct.GroupingItems());
      node->kind = output_order.has_value()
                       ? PhysicalOperatorKind::kOrderedDistinct
                       : PhysicalOperatorKind::kHashDistinct;
      node->provided_order = output_order.value_or(PhysicalOrdering{});
      return;
    }

    if (plan.Type() == ir::LogicalPlanNodeType::kAggregation) {
      RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
               "Aggregation physical node must have one child");
      const auto &aggregation = static_cast<const ir::AggregationPlan &>(plan);
      const auto output_order = OrderedGroupingOutput(
          node->children[0]->provided_order, aggregation.GroupingItems());
      node->kind = output_order.has_value()
                       ? PhysicalOperatorKind::kOrderedAggregation
                       : PhysicalOperatorKind::kHashAggregation;
      node->provided_order = output_order.value_or(PhysicalOrdering{});
      return;
    }
    node->kind = SelectOperatorKind(plan);
  }

  void BuildOperatorData(const ir::LogicalPlan &plan, PhysicalPlanNode *node) {
    RG_CHECK(node != nullptr, common::ErrorCode::InternalError,
             "physical plan node is null");
    switch (node->kind) {
      case PhysicalOperatorKind::kArgument:
        node->data = ArgumentOp{};
        return;
      case PhysicalOperatorKind::kAllNodeScan: {
        const auto &scan = static_cast<const ir::AllNodeScanPlan &>(plan);
        node->data = AllNodeScanOp{
            .variable = scan.Variable(),
            .output_slot = node->output_slots->At(scan.Variable())};
        return;
      }
      case PhysicalOperatorKind::kNodeByLabelScan: {
        const auto &scan = static_cast<const ir::NodeByLabelScanPlan &>(plan);
        node->data = NodeByLabelScanOp{
            .variable = scan.Variable(),
            .output_slot = node->output_slots->At(scan.Variable()),
            .labels = scan.Labels()};
        return;
      }
      case PhysicalOperatorKind::kNodeIndexSeek: {
        const auto &seek = static_cast<const ir::NodeIndexSeekPlan &>(plan);
        node->data = NodeIndexSeekOp{
            .variable = seek.Variable(),
            .output_slot = node->output_slots->At(seek.Variable()),
            .labels = seek.Labels(),
            .property_key = seek.PropertyKey(),
            .value = CopyPhysicalExpression(seek.ValueExpression(), {}),
            .unique = seek.Unique()};
        return;
      }
      case PhysicalOperatorKind::kNodeIndexRangeSeek: {
        const auto &seek =
            static_cast<const ir::NodeIndexRangeSeekPlan &>(plan);
        node->data = NodeIndexRangeSeekOp{
            .variable = seek.Variable(),
            .output_slot = node->output_slots->At(seek.Variable()),
            .labels = seek.Labels(),
            .property_key = seek.PropertyKey(),
            .predicates = CopyPhysicalExpressions(seek.Predicates())};
        return;
      }
      case PhysicalOperatorKind::kRelationshipTypeScan: {
        const auto &scan =
            static_cast<const ir::RelationshipTypeScanPlan &>(plan);
        PhysicalRelationshipPattern pattern{
            .from_node = scan.FromNode(),
            .relationship = scan.Relationship(),
            .to_node = scan.ToNode(),
            .direction = ToPhysicalExpandDirection(scan.Direction()),
            .types = scan.Types()};
        node->data =
            RelationshipTypeScanOp{.pattern = pattern,
                                   .slots = ResolveRelationshipOutputSlots(
                                       pattern, *node->output_slots)};
        return;
      }
      case PhysicalOperatorKind::kRelationshipIndexSeek: {
        const auto &seek =
            static_cast<const ir::RelationshipIndexSeekPlan &>(plan);
        PhysicalRelationshipPattern pattern{
            .from_node = seek.FromNode(),
            .relationship = seek.Relationship(),
            .to_node = seek.ToNode(),
            .direction = ToPhysicalExpandDirection(seek.Direction()),
            .types = seek.Types()};
        node->data = RelationshipIndexSeekOp{
            .pattern = pattern,
            .slots =
                ResolveRelationshipOutputSlots(pattern, *node->output_slots),
            .property_key = seek.PropertyKey(),
            .value = CopyPhysicalExpression(seek.ValueExpression(), {}),
            .unique = seek.Unique()};
        return;
      }
      case PhysicalOperatorKind::kRelationshipIndexRangeSeek: {
        const auto &seek =
            static_cast<const ir::RelationshipIndexRangeSeekPlan &>(plan);
        PhysicalRelationshipPattern pattern{
            .from_node = seek.FromNode(),
            .relationship = seek.Relationship(),
            .to_node = seek.ToNode(),
            .direction = ToPhysicalExpandDirection(seek.Direction()),
            .types = seek.Types()};
        node->data = RelationshipIndexRangeSeekOp{
            .pattern = pattern,
            .slots =
                ResolveRelationshipOutputSlots(pattern, *node->output_slots),
            .property_key = seek.PropertyKey(),
            .predicates = CopyPhysicalExpressions(seek.Predicates())};
        return;
      }
      case PhysicalOperatorKind::kNodeByIdSeek: {
        const auto &seek = static_cast<const ir::NodeByIdSeekPlan &>(plan);
        node->data = NodeByIdSeekOp{
            .variable = seek.Variable(),
            .output_slot = node->output_slots->At(seek.Variable()),
            .ids = CopyPhysicalExpression(seek.Ids(), {}),
            .many = seek.Many()};
        return;
      }
      case PhysicalOperatorKind::kRelationshipByIdSeek: {
        const auto &seek =
            static_cast<const ir::RelationshipByIdSeekPlan &>(plan);
        PhysicalRelationshipPattern pattern =
            CopyPhysicalRelationshipPattern(seek.Pattern());
        node->data = RelationshipByIdSeekOp{
            .pattern = pattern,
            .slots =
                ResolveRelationshipOutputSlots(pattern, *node->output_slots),
            .ids = CopyPhysicalExpression(seek.Ids(), {}),
            .many = seek.Many()};
        return;
      }
      case PhysicalOperatorKind::kExpand: {
        const auto &expand = static_cast<const ir::ExpandPlan &>(plan);
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "Expand physical node must have one child");
        node->data = ExpandOp{
            .pattern = {.from_node = expand.FromNode(),
                        .relationship = expand.Relationship(),
                        .to_node = expand.ToNode(),
                        .direction =
                            ToPhysicalExpandDirection(expand.Direction()),
                        .types = expand.Types()},
            .from_node_input_slot =
                node->children[0]->output_slots->At(expand.FromNode()),
            .relationship_output_slot =
                node->output_slots->At(expand.Relationship()),
            .to_node_output_slot = node->output_slots->At(expand.ToNode())};
        return;
      }
      case PhysicalOperatorKind::kExpandInto: {
        const auto &expand = static_cast<const ir::ExpandIntoPlan &>(plan);
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "ExpandInto physical node must have one child");
        node->data = ExpandIntoOp{
            .pattern = {.from_node = expand.FromNode(),
                        .relationship = expand.Relationship(),
                        .to_node = expand.ToNode(),
                        .direction =
                            ToPhysicalExpandDirection(expand.Direction()),
                        .types = expand.Types()},
            .from_node_input_slot =
                node->children[0]->output_slots->At(expand.FromNode()),
            .to_node_input_slot =
                node->children[0]->output_slots->At(expand.ToNode()),
            .relationship_output_slot =
                node->output_slots->At(expand.Relationship())};
        return;
      }
      case PhysicalOperatorKind::kVarExpand: {
        const auto &expand = static_cast<const ir::VarExpandPlan &>(plan);
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "VarExpand physical node must have one child");
        node->data = VarExpandOp{
            .pattern = {.from_node = expand.FromNode(),
                        .relationship = expand.Relationship(),
                        .to_node = expand.ToNode(),
                        .direction =
                            ToPhysicalExpandDirection(expand.Direction()),
                        .types = expand.Types()},
            .length = {.variable = true,
                       .min = expand.Length().min,
                       .max = expand.Length().max},
            .from_node_input_slot =
                node->children[0]->output_slots->At(expand.FromNode()),
            .to_node_input_slot =
                FindSlot(*node->children[0]->output_slots, expand.ToNode()),
            .relationship_output_slot =
                node->output_slots->At(expand.Relationship()),
            .to_node_output_slot = node->output_slots->At(expand.ToNode()),
            .reverse_relationships = expand.ReverseRelationships()};
        return;
      }
      case PhysicalOperatorKind::kPruningVarExpand: {
        const auto &expand =
            static_cast<const ir::PruningVarExpandPlan &>(plan);
        const ir::PatternRelationship &pattern = expand.Pattern();
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "PruningVarExpand physical node must have one child");
        node->data = PruningVarExpandOp{
            .pattern = CopyPhysicalRelationshipPattern(pattern),
            .length = {.variable = pattern.length.variable,
                       .min = pattern.length.min,
                       .max = pattern.length.max},
            .from_node_input_slot =
                node->children[0]->output_slots->At(pattern.left_node),
            .to_node_output_slot = node->output_slots->At(pattern.right_node)};
        return;
      }
      case PhysicalOperatorKind::kOptionalExpand: {
        const auto &expand = static_cast<const ir::OptionalExpandPlan &>(plan);
        const ir::PatternRelationship &pattern = expand.Pattern();
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "OptionalExpand physical node must have one child");
        OptionalExpandOp data{
            .pattern = CopyPhysicalRelationshipPattern(pattern),
            .predicates = CopyPhysicalExpressions(expand.Predicates()),
            .from_node_input_slot =
                node->children[0]->output_slots->At(pattern.left_node),
            .relationship_output_slot =
                node->output_slots->At(pattern.variable),
            .to_node_output_slot = node->output_slots->At(pattern.right_node)};
        data.output_slots.reserve(node->output_slots->Columns().size());
        for (const auto &column : node->output_slots->Columns()) {
          data.output_slots.push_back(node->output_slots->At(column));
        }
        node->data = std::move(data);
        return;
      }
      case PhysicalOperatorKind::kProjectEndpoints: {
        const auto &project =
            static_cast<const ir::ProjectEndpointsPlan &>(plan);
        const ir::PatternRelationship &pattern = project.Pattern();
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "ProjectEndpoints physical node must have one child");
        node->data = ProjectEndpointsOp{
            .pattern = CopyPhysicalRelationshipPattern(pattern),
            .length = {.variable = pattern.length.variable,
                       .min = pattern.length.min,
                       .max = pattern.length.max},
            .relationship_input_slot =
                node->children[0]->output_slots->At(pattern.variable),
            .from_node_input_slot =
                FindSlot(*node->children[0]->output_slots, pattern.left_node),
            .to_node_input_slot =
                FindSlot(*node->children[0]->output_slots, pattern.right_node),
            .from_node_output_slot = node->output_slots->At(pattern.left_node),
            .to_node_output_slot = node->output_slots->At(pattern.right_node)};
        return;
      }
      case PhysicalOperatorKind::kPathBuild: {
        const auto &build = static_cast<const ir::PathBuildPlan &>(plan);
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "PathBuild physical node must have one child");
        PathBuildOp data{
            .path = {.variable = build.Path().variable,
                     .nodes = build.Path().nodes,
                     .relationships = build.Path().relationships},
            .path_output_slot = node->output_slots->At(build.Path().variable)};
        data.node_input_slots.reserve(build.Path().nodes.size());
        for (const auto &name : build.Path().nodes) {
          data.node_input_slots.push_back(
              node->children[0]->output_slots->At(name));
        }
        data.relationship_input_slots.reserve(
            build.Path().relationships.size());
        for (const auto &name : build.Path().relationships) {
          data.relationship_input_slots.push_back(
              node->children[0]->output_slots->At(name));
        }
        node->data = std::move(data);
        return;
      }
      case PhysicalOperatorKind::kFilter: {
        const auto &filter = static_cast<const ir::FilterPlan &>(plan);
        node->data =
            FilterOp{.predicate = CopyPhysicalExpression(
                         filter.Predicate(), filter.PrecomputedExpressions())};
        return;
      }
      case PhysicalOperatorKind::kProjection: {
        const auto &projection = static_cast<const ir::ProjectionPlan &>(plan);
        ProjectionOp data;
        data.items.reserve(projection.Items().size());
        for (const auto &item : projection.Items()) {
          PhysicalProjectionItem physical_item{
              .alias = item.alias,
              .output_slot = node->output_slots->At(item.alias),
              .passthrough = item.passthrough};
          if (item.passthrough) {
            RG_CHECK(node->children.size() == 1,
                     common::ErrorCode::InternalError,
                     "passthrough projection must have one child");
            physical_item.source_slot =
                node->children.front()->output_slots->Find(item.alias);
            RG_CHECK(
                physical_item.source_slot.has_value(),
                common::ErrorCode::InternalError,
                "passthrough projection source slot is missing: " + item.alias);
          }
          if (!item.passthrough) {
            physical_item.expression = CopyPhysicalExpression(
                item.expression, item.precomputed_expressions);
          }
          data.items.push_back(std::move(physical_item));
        }
        node->data = std::move(data);
        return;
      }
      case PhysicalOperatorKind::kHashDistinct: {
        const auto &distinct = static_cast<const ir::DistinctPlan &>(plan);
        node->data =
            HashDistinctOp{.grouping_items = CopyPhysicalGroupingItems(
                               distinct.GroupingItems(), *node->output_slots)};
        return;
      }
      case PhysicalOperatorKind::kOrderedDistinct: {
        const auto &distinct = static_cast<const ir::DistinctPlan &>(plan);
        node->data = OrderedDistinctOp{
            .grouping_items = CopyPhysicalGroupingItems(
                distinct.GroupingItems(), *node->output_slots)};
        return;
      }
      case PhysicalOperatorKind::kHashAggregation: {
        const auto &aggregation =
            static_cast<const ir::AggregationPlan &>(plan);
        node->data = HashAggregationOp{
            .grouping_items = CopyPhysicalGroupingItems(
                aggregation.GroupingItems(), *node->output_slots),
            .aggregation_items = CopyPhysicalAggregationItems(
                aggregation.AggregationItems(), *node->output_slots)};
        return;
      }
      case PhysicalOperatorKind::kOrderedAggregation: {
        const auto &aggregation =
            static_cast<const ir::AggregationPlan &>(plan);
        node->data = OrderedAggregationOp{
            .grouping_items = CopyPhysicalGroupingItems(
                aggregation.GroupingItems(), *node->output_slots),
            .aggregation_items = CopyPhysicalAggregationItems(
                aggregation.AggregationItems(), *node->output_slots)};
        return;
      }
      case PhysicalOperatorKind::kFullSort: {
        const auto &sort = static_cast<const ir::SortPlan &>(plan);
        node->data = FullSortOp{.items = CopyPhysicalSortItems(sort.Items())};
        return;
      }
      case PhysicalOperatorKind::kPartialSort: {
        const auto &sort = static_cast<const ir::SortPlan &>(plan);
        node->data = PartialSortOp{
            .items = CopyPhysicalSortItems(sort.Items()),
            .prefix = CommonOrderingPrefix(node->children[0]->provided_order,
                                           node->provided_order)};
        return;
      }
      case PhysicalOperatorKind::kTopN: {
        const auto &top_n = static_cast<const ir::TopNPlan &>(plan);
        node->data = TopNOp{.items = CopyPhysicalSortItems(top_n.Items()),
                            .limit = CopyPhysicalExpression(
                                top_n.Limit(), top_n.PrecomputedExpressions())};
        return;
      }
      case PhysicalOperatorKind::kPartialTopN: {
        const auto &top_n = static_cast<const ir::TopNPlan &>(plan);
        node->data = PartialTopNOp{
            .items = CopyPhysicalSortItems(top_n.Items()),
            .limit = CopyPhysicalExpression(top_n.Limit(),
                                            top_n.PrecomputedExpressions()),
            .prefix = CommonOrderingPrefix(node->children[0]->provided_order,
                                           node->provided_order)};
        return;
      }
      case PhysicalOperatorKind::kSkip: {
        const auto &skip = static_cast<const ir::SkipPlan &>(plan);
        node->data = SkipOp{.count = CopyPhysicalExpression(
                                skip.Skip(), skip.PrecomputedExpressions())};
        return;
      }
      case PhysicalOperatorKind::kLimit: {
        const auto &limit = static_cast<const ir::LimitPlan &>(plan);
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "limit physical node must have one child");
        node->data =
            LimitOp{.count = CopyPhysicalExpression(
                        limit.Limit(), limit.PrecomputedExpressions()),
                    .exhaust_child = node->children[0]->subtree_effects.writes};
        return;
      }
      case PhysicalOperatorKind::kProduceResults: {
        std::vector<std::string> columns = plan.OutputColumns();
        node->data = ProduceResultsOp{
            .columns = columns,
            .output_slots = ResolveSlots(*node->output_slots, columns)};
        return;
      }
      case PhysicalOperatorKind::kAssertIsNode: {
        const auto &assertion = static_cast<const ir::AssertIsNodePlan &>(plan);
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "assert-is-node physical node must have one child");
        AssertIsNodeOp data;
        data.nodes.reserve(assertion.Variables().size());
        for (const auto &variable : assertion.Variables()) {
          data.nodes.push_back(
              {.variable = variable,
               .input_slot = node->children[0]->output_slots->At(variable)});
        }
        node->data = std::move(data);
        return;
      }
      case PhysicalOperatorKind::kUnwind: {
        const auto &unwind = static_cast<const ir::UnwindPlan &>(plan);
        node->data = UnwindOp{
            .expression = CopyPhysicalExpression(unwind.Expression(), {}),
            .value_slot = node->output_slots->At(unwind.Alias())};
        return;
      }
      case PhysicalOperatorKind::kProcedureCall: {
        const auto &call = static_cast<const ir::ProcedureCallPlan &>(plan);
        ProcedureCallOp data{
            .procedure_name = call.ProcedureName(),
            .arguments = CopyPhysicalExpressions(call.Arguments()),
            .read_only = call.ReadOnly()};
        data.yields.reserve(call.YieldItems().size());
        for (const auto &item : call.YieldItems()) {
          data.yields.push_back(
              {.result_field = item.result_field.has_value()
                                   ? *item.result_field
                                   : item.variable,
               .output_slot = node->output_slots->At(item.variable)});
        }
        node->data = std::move(data);
        return;
      }
      case PhysicalOperatorKind::kCartesianProduct:
        node->data = CartesianProductOp{};
        return;
      case PhysicalOperatorKind::kNodeHashJoin: {
        const auto &join = static_cast<const ir::NodeHashJoinPlan &>(plan);
        RG_CHECK(node->children.size() == 2, common::ErrorCode::InternalError,
                 "node hash join physical node must have two children");
        NodeHashJoinOp data{
            .join_keys = join.JoinKeys(),
            .left_key_slots =
                ResolveSlots(*node->children[0]->output_slots, join.JoinKeys()),
            .right_key_slots = ResolveSlots(*node->children[1]->output_slots,
                                            join.JoinKeys())};
        const auto &left_rows = plan.Child(0).EstimatedRows();
        const auto &right_rows = plan.Child(1).EstimatedRows();
        if (left_rows.has_value() && right_rows.has_value() &&
            *left_rows < *right_rows) {
          data.build_child = 0;
        }
        node->data = std::move(data);
        return;
      }
      case PhysicalOperatorKind::kValueHashJoin: {
        const auto &join = static_cast<const ir::ValueHashJoinPlan &>(plan);
        ValueHashJoinOp data;
        data.keys.reserve(join.JoinKeys().size());
        for (const auto &key : join.JoinKeys()) {
          data.keys.push_back({.left = CopyPhysicalExpression(key.left, {}),
                               .right = CopyPhysicalExpression(key.right, {})});
        }
        data.predicates = CopyPhysicalExpressions(join.Predicates());
        const auto &left_rows = plan.Child(0).EstimatedRows();
        const auto &right_rows = plan.Child(1).EstimatedRows();
        if (left_rows.has_value() && right_rows.has_value() &&
            *left_rows < *right_rows) {
          data.build_child = 0;
        }
        node->data = std::move(data);
        return;
      }
      case PhysicalOperatorKind::kPredicateJoin: {
        const auto &join = static_cast<const ir::PredicateJoinPlan &>(plan);
        node->data = PredicateJoinOp{
            .predicates = CopyPhysicalExpressions(join.Predicates())};
        return;
      }
      case PhysicalOperatorKind::kLeftOuterHashJoin: {
        const auto &join = static_cast<const ir::LeftOuterHashJoinPlan &>(plan);
        RG_CHECK(node->children.size() == 2, common::ErrorCode::InternalError,
                 "left outer hash join physical node must have two children");
        node->data = LeftOuterHashJoinOp{
            .join_keys = join.JoinKeys(),
            .left_key_slots =
                ResolveSlots(*node->children[0]->output_slots, join.JoinKeys()),
            .right_key_slots =
                ResolveSlots(*node->children[1]->output_slots, join.JoinKeys()),
            .nullable_output_slots = AllSlots(*node->output_slots)};
        return;
      }
      case PhysicalOperatorKind::kUnionAll:
        node->data = UnionAllOp{};
        return;
      case PhysicalOperatorKind::kUnionDistinct: {
        UnionDistinctOp data;
        data.key_slots.reserve(node->output_slots->Columns().size());
        for (const auto &column : node->output_slots->Columns()) {
          data.key_slots.push_back(node->output_slots->At(column));
        }
        node->data = std::move(data);
        return;
      }
      case PhysicalOperatorKind::kApply:
        node->data = ApplyOp{};
        return;
      case PhysicalOperatorKind::kOptionalApply:
        node->data = OptionalApplyOp{.nullable_output_slots =
                                         AllSlots(*node->output_slots)};
        return;
      case PhysicalOperatorKind::kSemiApply:
        node->data = SemiApplyOp{};
        return;
      case PhysicalOperatorKind::kAntiSemiApply:
        node->data = AntiSemiApplyOp{};
        return;
      case PhysicalOperatorKind::kLetSemiApply: {
        const auto &apply = static_cast<const ir::LetSemiApplyPlan &>(plan);
        node->data = LetSemiApplyOp{
            .value_slot = node->output_slots->At(apply.ValueVariable())};
        return;
      }
      case PhysicalOperatorKind::kSelectOrSemiApply: {
        const auto &apply =
            static_cast<const ir::SelectOrSemiApplyPlan &>(plan);
        node->data = SelectOrSemiApplyOp{
            .predicate = CopyPhysicalExpression(apply.Predicate(), {}),
            .anti = apply.Anti()};
        return;
      }
      case PhysicalOperatorKind::kRollUpApply: {
        const auto &apply = static_cast<const ir::RollUpApplyPlan &>(plan);
        RG_CHECK(node->children.size() == 2, common::ErrorCode::InternalError,
                 "roll-up apply physical node must have two children");
        node->data = RollUpApplyOp{
            .collection_slot =
                node->output_slots->At(apply.CollectionVariable()),
            .value_slot =
                node->children[1]->output_slots->At(apply.ValueVariable())};
        return;
      }
      case PhysicalOperatorKind::kWriteBarrier:
        node->data = WriteBarrierOp{};
        return;
      case PhysicalOperatorKind::kCreateNode: {
        const auto &create = static_cast<const ir::CreateNodePlan &>(plan);
        const ir::CreateNodePattern &node_pattern = create.Node();
        node->data = CreateNodeOp{
            .node_slot = node->output_slots->At(node_pattern.variable),
            .labels = node_pattern.labels,
            .properties = CopyPhysicalPropertyMap(node_pattern.properties)};
        return;
      }
      case PhysicalOperatorKind::kCreateRelationship: {
        const auto &create =
            static_cast<const ir::CreateRelationshipPlan &>(plan);
        const ir::CreateRelationshipPattern &relationship =
            create.Relationship();
        RG_CHECK(node->children.size() == 1, common::ErrorCode::InternalError,
                 "create relationship physical node must have one child");
        const bool incoming =
            relationship.direction == ir::Direction::kIncoming;
        node->data = CreateRelationshipOp{
            .relationship_slot = node->output_slots->At(relationship.variable),
            .left_node_slot = node->children[0]->output_slots->At(
                incoming ? relationship.right_node : relationship.left_node),
            .right_node_slot = node->children[0]->output_slots->At(
                incoming ? relationship.left_node : relationship.right_node),
            .type = relationship.types.empty() ? std::string()
                                               : relationship.types.front(),
            .properties = CopyPhysicalPropertyMap(relationship.properties)};
        return;
      }
      case PhysicalOperatorKind::kMerge: {
        const auto &merge = static_cast<const ir::MergePlan &>(plan);
        node->data = CopyPhysicalMerge(merge.Merge(), *node->output_slots);
        return;
      }
      case PhysicalOperatorKind::kSetProperty: {
        const auto &set = static_cast<const ir::SetPropertyPlan &>(plan);
        node->data =
            SetPropertyOp{.entity = CopyPhysicalExpression(set.Entity(), {}),
                          .property_key = set.PropertyKey(),
                          .value = CopyPhysicalExpression(set.Value(), {})};
        return;
      }
      case PhysicalOperatorKind::kSetProperties: {
        const auto &set = static_cast<const ir::SetPropertiesPlan &>(plan);
        node->data =
            SetPropertiesOp{.entity = CopyPhysicalExpression(set.Entity(), {}),
                            .value = CopyPhysicalExpression(set.Value(), {}),
                            .include_existing = set.IncludeExisting()};
        return;
      }
      case PhysicalOperatorKind::kSetLabels: {
        const auto &set = static_cast<const ir::SetLabelsPlan &>(plan);
        node->data =
            SetLabelsOp{.entity = CopyPhysicalExpression(set.Entity(), {}),
                        .labels = set.Labels()};
        return;
      }
      case PhysicalOperatorKind::kRemoveProperty: {
        const auto &remove = static_cast<const ir::RemovePropertyPlan &>(plan);
        node->data = RemovePropertyOp{
            .entity = CopyPhysicalExpression(remove.Entity(), {}),
            .property_key = remove.PropertyKey()};
        return;
      }
      case PhysicalOperatorKind::kRemoveLabels: {
        const auto &remove = static_cast<const ir::RemoveLabelsPlan &>(plan);
        node->data = RemoveLabelsOp{
            .entity = CopyPhysicalExpression(remove.Entity(), {}),
            .labels = remove.Labels()};
        return;
      }
      case PhysicalOperatorKind::kDelete: {
        const auto &del = static_cast<const ir::DeletePlan &>(plan);
        node->data =
            DeleteOp{.expressions = CopyPhysicalExpressions(del.Expressions())};
        return;
      }
      case PhysicalOperatorKind::kDetachDelete: {
        const auto &del = static_cast<const ir::DetachDeletePlan &>(plan);
        node->data = DetachDeleteOp{
            .expressions = CopyPhysicalExpressions(del.Expressions())};
        return;
      }
      default:
        return;
    }
  }

  void BuildUnionLayout(const ir::UnionPlan &plan, PhysicalPlanNode *node) {
    RG_CHECK(node != nullptr && node->children.size() == 2,
             common::ErrorCode::InternalError,
             "union physical node is incomplete");
    std::vector<std::pair<std::string, std::string>> left_names;
    std::vector<std::pair<std::string, std::string>> right_names;
    for (const auto &mapping : plan.Mappings()) {
      left_names.emplace_back(mapping.lhs_variable, mapping.output_variable);
      right_names.emplace_back(mapping.rhs_variable, mapping.output_variable);
    }
    node->output_slots = MakeLayout(plan.OutputColumns());
    node->child_mappings.push_back(NamedMappings(
        *node->children[0]->output_slots, *node->output_slots, left_names));
    node->child_mappings.push_back(NamedMappings(
        *node->children[1]->output_slots, *node->output_slots, right_names));
  }

  OperatorId next_id_ = 0;
  SlotConfigurationPtr empty_slots_;
};

}  // namespace

std::string_view ToString(PhysicalOperatorKind kind) {
  switch (kind) {
    case PhysicalOperatorKind::kArgument:
      return "Argument";
    case PhysicalOperatorKind::kAllNodeScan:
      return "AllNodeScan";
    case PhysicalOperatorKind::kNodeByLabelScan:
      return "NodeByLabelScan";
    case PhysicalOperatorKind::kNodeIndexSeek:
      return "NodeIndexSeek";
    case PhysicalOperatorKind::kNodeIndexRangeSeek:
      return "NodeIndexRangeSeek";
    case PhysicalOperatorKind::kRelationshipTypeScan:
      return "RelationshipTypeScan";
    case PhysicalOperatorKind::kRelationshipIndexSeek:
      return "RelationshipIndexSeek";
    case PhysicalOperatorKind::kRelationshipIndexRangeSeek:
      return "RelationshipIndexRangeSeek";
    case PhysicalOperatorKind::kExpand:
      return "Expand";
    case PhysicalOperatorKind::kExpandInto:
      return "ExpandInto";
    case PhysicalOperatorKind::kVarExpand:
      return "VarExpand";
    case PhysicalOperatorKind::kPathBuild:
      return "PathBuild";
    case PhysicalOperatorKind::kFilter:
      return "Filter";
    case PhysicalOperatorKind::kProjection:
      return "Projection";
    case PhysicalOperatorKind::kHashDistinct:
      return "HashDistinct";
    case PhysicalOperatorKind::kOrderedDistinct:
      return "OrderedDistinct";
    case PhysicalOperatorKind::kHashAggregation:
      return "HashAggregation";
    case PhysicalOperatorKind::kOrderedAggregation:
      return "OrderedAggregation";
    case PhysicalOperatorKind::kFullSort:
      return "FullSort";
    case PhysicalOperatorKind::kPartialSort:
      return "PartialSort";
    case PhysicalOperatorKind::kSkip:
      return "Skip";
    case PhysicalOperatorKind::kLimit:
      return "Limit";
    case PhysicalOperatorKind::kTopN:
      return "TopN";
    case PhysicalOperatorKind::kPartialTopN:
      return "PartialTopN";
    case PhysicalOperatorKind::kProduceResults:
      return "ProduceResults";
    case PhysicalOperatorKind::kCartesianProduct:
      return "CartesianProduct";
    case PhysicalOperatorKind::kNodeHashJoin:
      return "NodeHashJoin";
    case PhysicalOperatorKind::kValueHashJoin:
      return "ValueHashJoin";
    case PhysicalOperatorKind::kPredicateJoin:
      return "PredicateJoin";
    case PhysicalOperatorKind::kApply:
      return "Apply";
    case PhysicalOperatorKind::kSemiApply:
      return "SemiApply";
    case PhysicalOperatorKind::kAntiSemiApply:
      return "AntiSemiApply";
    case PhysicalOperatorKind::kLetSemiApply:
      return "LetSemiApply";
    case PhysicalOperatorKind::kRollUpApply:
      return "RollUpApply";
    case PhysicalOperatorKind::kOptionalApply:
      return "OptionalApply";
    case PhysicalOperatorKind::kAssertIsNode:
      return "AssertIsNode";
    case PhysicalOperatorKind::kWriteBarrier:
      return "WriteBarrier";
    case PhysicalOperatorKind::kCreateNode:
      return "CreateNode";
    case PhysicalOperatorKind::kCreateRelationship:
      return "CreateRelationship";
    case PhysicalOperatorKind::kMerge:
      return "Merge";
    case PhysicalOperatorKind::kSetProperty:
      return "SetProperty";
    case PhysicalOperatorKind::kSetProperties:
      return "SetProperties";
    case PhysicalOperatorKind::kSetLabels:
      return "SetLabels";
    case PhysicalOperatorKind::kRemoveProperty:
      return "RemoveProperty";
    case PhysicalOperatorKind::kRemoveLabels:
      return "RemoveLabels";
    case PhysicalOperatorKind::kDelete:
      return "Delete";
    case PhysicalOperatorKind::kDetachDelete:
      return "DetachDelete";
    case PhysicalOperatorKind::kUnwind:
      return "Unwind";
    case PhysicalOperatorKind::kProcedureCall:
      return "ProcedureCall";
    case PhysicalOperatorKind::kNodeByIdSeek:
      return "NodeByIdSeek";
    case PhysicalOperatorKind::kRelationshipByIdSeek:
      return "RelationshipByIdSeek";
    case PhysicalOperatorKind::kProjectEndpoints:
      return "ProjectEndpoints";
    case PhysicalOperatorKind::kOptionalExpand:
      return "OptionalExpand";
    case PhysicalOperatorKind::kLeftOuterHashJoin:
      return "LeftOuterHashJoin";
    case PhysicalOperatorKind::kSelectOrSemiApply:
      return "SelectOrSemiApply";
    case PhysicalOperatorKind::kPruningVarExpand:
      return "PruningVarExpand";
    case PhysicalOperatorKind::kUnionAll:
      return "UnionAll";
    case PhysicalOperatorKind::kUnionDistinct:
      return "UnionDistinct";
  }
  RG_THROW(common::ErrorCode::InternalError, "unknown physical operator kind");
}

std::string_view ToString(PhysicalSortDirection direction) {
  switch (direction) {
    case PhysicalSortDirection::kAscending:
      return "ASC";
    case PhysicalSortDirection::kDescending:
      return "DESC";
  }
  RG_THROW(common::ErrorCode::InternalError, "unknown physical sort direction");
}

PhysicalPlan::PhysicalPlan(std::unique_ptr<PhysicalPlanNode> root,
                           std::vector<std::string> result_columns)
    : root_(std::move(root)), result_columns_(std::move(result_columns)) {
  RG_CHECK(root_ != nullptr, common::ErrorCode::InvalidParameter,
           "physical plan root is null");
}

const PhysicalPlanNode &PhysicalPlan::Root() const { return *root_; }

const PhysicalPlanEffects &PhysicalPlan::Effects() const noexcept {
  return root_->subtree_effects;
}

const std::vector<std::string> &PhysicalPlan::ResultColumns() const noexcept {
  return result_columns_;
}

PhysicalPlan CreatePhysicalPlan(const ir::LogicalPlan &plan) {
  return PhysicalPlanBuilder().Build(plan);
}

}  // namespace rg
