#include "runtime/physical_plan.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "ast/ast_clone.h"
#include "ast/ast_equal.h"
#include "ast/ast_node.h"
#include "ast/builtin_function.h"
#include "ast/builtin_procedure.h"
#include "ast/expression_dependency.h"
#include "common/exception.h"

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

using SemanticType = ast::SemanticVariableType;

struct TypeInfo {
  SemanticType type = SemanticType::kUnknown;
  bool nullable = true;
  std::optional<SlotKind> storage_kind;
};

using TypeOverrides = std::unordered_map<std::string, TypeInfo>;

PhysicalSortDirection ToPhysicalSortDirection(
    ir::LogicalOrderDirection direction) {
  switch (direction) {
    case ir::LogicalOrderDirection::kAscending:
      return PhysicalSortDirection::kAscending;
    case ir::LogicalOrderDirection::kDescending:
      return PhysicalSortDirection::kDescending;
  }
  THROW(common::InternalError, "unknown sort direction");
}

PhysicalOrdering CopyPhysicalOrdering(
    const std::vector<ir::LogicalSortItem> &items) {
  PhysicalOrdering copied;
  copied.reserve(items.size());
  for (const auto &item : items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "physical ordering expression is null");
    copied.push_back({.expression = std::shared_ptr<const ast::Expression>(
                          ast::CloneExpression(*item.expression)),
                      .direction = ToPhysicalSortDirection(item.direction)});
  }
  return copied;
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

TypeInfo InfoForSlot(const Slot &slot) {
  return {
      .type = slot.type, .nullable = slot.nullable, .storage_kind = slot.kind};
}

TypeInfo MergeInfo(TypeInfo left, TypeInfo right) {
  const bool nullable = left.nullable || right.nullable;
  if (left.type == right.type) {
    return {
        .type = left.type,
        .nullable = nullable,
        .storage_kind = left.storage_kind == right.storage_kind
                            ? left.storage_kind
                            : std::optional<SlotKind>(SlotKind::kReference)};
  }
  if (left.type == SemanticType::kUnknown ||
      right.type == SemanticType::kUnknown) {
    return {.type = SemanticType::kUnknown,
            .nullable = nullable,
            .storage_kind = SlotKind::kReference};
  }
  return {.type = SemanticType::kUnknown,
          .nullable = nullable,
          .storage_kind = SlotKind::kReference};
}

std::optional<TypeInfo> FindInfo(
    std::string_view name, const std::vector<SlotConfigurationPtr> &sources) {
  std::optional<TypeInfo> result;
  for (const auto &source : sources) {
    if (source == nullptr) {
      continue;
    }
    const Slot *slot = source->Find(name);
    if (slot == nullptr) {
      continue;
    }
    result = result.has_value() ? MergeInfo(*result, InfoForSlot(*slot))
                                : InfoForSlot(*slot);
  }
  return result;
}

TypeInfo InferExpression(const ast::Expression *expression,
                         const std::vector<SlotConfigurationPtr> &sources) {
  if (expression == nullptr) {
    return {};
  }
  switch (expression->node_type) {
    case ast::ASTNodeType::kVariable: {
      const auto &variable = ast::CastAst<ast::Variable>(*expression);
      return FindInfo(variable.name, sources).value_or(TypeInfo{});
    }
    case ast::ASTNodeType::kBooleanLiteral:
    case ast::ASTNodeType::kIntegerLiteral:
    case ast::ASTNodeType::kDoubleLiteral:
    case ast::ASTNodeType::kStringLiteral:
      return {.type = SemanticType::kScalar, .nullable = false};
    case ast::ASTNodeType::kNullLiteral:
      return {.type = SemanticType::kUnknown, .nullable = true};
    case ast::ASTNodeType::kListLiteral:
    case ast::ASTNodeType::kListComprehension:
    case ast::ASTNodeType::kPatternComprehension:
      return {.type = SemanticType::kList, .nullable = false};
    case ast::ASTNodeType::kMapLiteral:
      return {.type = SemanticType::kMap, .nullable = false};
    case ast::ASTNodeType::kFunctionInvocation: {
      const auto &function = ast::CastAst<ast::FunctionInvocation>(*expression);
      const ast::BuiltinFunction *builtin =
          ast::FindBuiltinFunction(function.function_name);
      return builtin == nullptr
                 ? TypeInfo{}
                 : TypeInfo{.type = builtin->result_type, .nullable = true};
    }
    case ast::ASTNodeType::kParenthesizedExpression:
      return InferExpression(
          ast::CastAst<ast::ParenthesizedExpression>(*expression).expr.get(),
          sources);
    default:
      return {.type = SemanticType::kScalar, .nullable = true};
  }
}

TypeInfo ProjectionInfo(const ir::LogicalProjectionItem &item,
                        const std::vector<SlotConfigurationPtr> &sources) {
  if (item.passthrough) {
    return FindInfo(item.alias, sources).value_or(TypeInfo{});
  }
  TypeInfo info = InferExpression(item.expression, sources);
  if (item.semantic_type == SemanticType::kUnknown) {
    return info;
  }
  info.type = item.semantic_type;
  if (item.expression == nullptr ||
      !item.expression->Is(ast::ASTNodeType::kVariable)) {
    info.storage_kind.reset();
  }
  return info;
}

void SetOverride(TypeOverrides *overrides, std::string_view name,
                 SemanticType type, bool nullable) {
  if (!name.empty()) {
    (*overrides)[std::string(name)] = {.type = type, .nullable = nullable};
  }
}

TypeOverrides OutputOverrides(
    const ir::LogicalPlan &plan,
    const std::vector<SlotConfigurationPtr> &sources) {
  TypeOverrides overrides;
  switch (plan.Type()) {
    case ir::LogicalPlanNodeType::kNodeByIdSeek:
      SetOverride(&overrides,
                  static_cast<const ir::NodeByIdSeekPlan &>(plan).Variable(),
                  SemanticType::kNode, false);
      break;
    case ir::LogicalPlanNodeType::kRelationshipByIdSeek:
    case ir::LogicalPlanNodeType::kProjectEndpoints:
    case ir::LogicalPlanNodeType::kOptionalExpand:
    case ir::LogicalPlanNodeType::kPruningVarExpand: {
      const ir::PatternRelationship *pattern = nullptr;
      if (plan.Type() == ir::LogicalPlanNodeType::kRelationshipByIdSeek) {
        pattern =
            &static_cast<const ir::RelationshipByIdSeekPlan &>(plan).Pattern();
      } else if (plan.Type() == ir::LogicalPlanNodeType::kProjectEndpoints) {
        pattern =
            &static_cast<const ir::ProjectEndpointsPlan &>(plan).Pattern();
      } else if (plan.Type() == ir::LogicalPlanNodeType::kOptionalExpand) {
        pattern = &static_cast<const ir::OptionalExpandPlan &>(plan).Pattern();
      } else {
        pattern =
            &static_cast<const ir::PruningVarExpandPlan &>(plan).Pattern();
      }
      const bool optional =
          plan.Type() == ir::LogicalPlanNodeType::kOptionalExpand;
      for (const auto &name : {pattern->left_node, pattern->right_node}) {
        const auto existing = FindInfo(name, sources);
        SetOverride(&overrides, name, SemanticType::kNode,
                    existing.has_value() ? existing->nullable : optional);
      }
      if (plan.Type() != ir::LogicalPlanNodeType::kPruningVarExpand) {
        SetOverride(&overrides, pattern->variable,
                    pattern->length.variable ? SemanticType::kList
                                             : SemanticType::kRelationship,
                    optional);
      }
      break;
    }
    case ir::LogicalPlanNodeType::kAllNodeScan:
      SetOverride(&overrides,
                  static_cast<const ir::AllNodeScanPlan &>(plan).Variable(),
                  SemanticType::kNode, false);
      break;
    case ir::LogicalPlanNodeType::kNodeByLabelScan:
      SetOverride(&overrides,
                  static_cast<const ir::NodeByLabelScanPlan &>(plan).Variable(),
                  SemanticType::kNode, false);
      break;
    case ir::LogicalPlanNodeType::kNodeIndexSeek:
      SetOverride(&overrides,
                  static_cast<const ir::NodeIndexSeekPlan &>(plan).Variable(),
                  SemanticType::kNode, false);
      break;
    case ir::LogicalPlanNodeType::kNodeIndexRangeSeek:
      SetOverride(
          &overrides,
          static_cast<const ir::NodeIndexRangeSeekPlan &>(plan).Variable(),
          SemanticType::kNode, false);
      break;
    case ir::LogicalPlanNodeType::kRelationshipTypeScan: {
      const auto &scan =
          static_cast<const ir::RelationshipTypeScanPlan &>(plan);
      SetOverride(&overrides, scan.FromNode(), SemanticType::kNode, false);
      SetOverride(&overrides, scan.Relationship(), SemanticType::kRelationship,
                  false);
      SetOverride(&overrides, scan.ToNode(), SemanticType::kNode, false);
      break;
    }
    case ir::LogicalPlanNodeType::kRelationshipIndexSeek: {
      const auto &scan =
          static_cast<const ir::RelationshipIndexSeekPlan &>(plan);
      SetOverride(&overrides, scan.FromNode(), SemanticType::kNode, false);
      SetOverride(&overrides, scan.Relationship(), SemanticType::kRelationship,
                  false);
      SetOverride(&overrides, scan.ToNode(), SemanticType::kNode, false);
      break;
    }
    case ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek: {
      const auto &scan =
          static_cast<const ir::RelationshipIndexRangeSeekPlan &>(plan);
      SetOverride(&overrides, scan.FromNode(), SemanticType::kNode, false);
      SetOverride(&overrides, scan.Relationship(), SemanticType::kRelationship,
                  false);
      SetOverride(&overrides, scan.ToNode(), SemanticType::kNode, false);
      break;
    }
    case ir::LogicalPlanNodeType::kExpand: {
      const auto &expand = static_cast<const ir::ExpandPlan &>(plan);
      SetOverride(
          &overrides, expand.FromNode(), SemanticType::kNode,
          FindInfo(expand.FromNode(), sources).value_or(TypeInfo{}).nullable);
      SetOverride(&overrides, expand.Relationship(),
                  SemanticType::kRelationship, false);
      SetOverride(&overrides, expand.ToNode(), SemanticType::kNode, false);
      break;
    }
    case ir::LogicalPlanNodeType::kExpandInto: {
      const auto &expand = static_cast<const ir::ExpandIntoPlan &>(plan);
      SetOverride(
          &overrides, expand.FromNode(), SemanticType::kNode,
          FindInfo(expand.FromNode(), sources).value_or(TypeInfo{}).nullable);
      SetOverride(&overrides, expand.Relationship(),
                  SemanticType::kRelationship, false);
      SetOverride(
          &overrides, expand.ToNode(), SemanticType::kNode,
          FindInfo(expand.ToNode(), sources).value_or(TypeInfo{}).nullable);
      break;
    }
    case ir::LogicalPlanNodeType::kVarExpand: {
      const auto &expand = static_cast<const ir::VarExpandPlan &>(plan);
      SetOverride(
          &overrides, expand.FromNode(), SemanticType::kNode,
          FindInfo(expand.FromNode(), sources).value_or(TypeInfo{}).nullable);
      SetOverride(&overrides, expand.Relationship(), SemanticType::kList,
                  false);
      SetOverride(&overrides, expand.ToNode(), SemanticType::kNode, false);
      break;
    }
    case ir::LogicalPlanNodeType::kPathBuild:
      SetOverride(&overrides,
                  static_cast<const ir::PathBuildPlan &>(plan).PathVariable(),
                  SemanticType::kPath, false);
      break;
    case ir::LogicalPlanNodeType::kProjection: {
      const auto &projection = static_cast<const ir::ProjectionPlan &>(plan);
      for (const auto &item : projection.Items()) {
        overrides[item.alias] = ProjectionInfo(item, sources);
      }
      break;
    }
    case ir::LogicalPlanNodeType::kDistinct: {
      const auto &distinct = static_cast<const ir::DistinctPlan &>(plan);
      for (const auto &item : distinct.GroupingItems()) {
        overrides[item.alias] = ProjectionInfo(item, sources);
      }
      break;
    }
    case ir::LogicalPlanNodeType::kAggregation: {
      const auto &aggregation = static_cast<const ir::AggregationPlan &>(plan);
      for (const auto &item : aggregation.GroupingItems()) {
        overrides[item.alias] = ProjectionInfo(item, sources);
      }
      for (const auto &item : aggregation.AggregationItems()) {
        overrides[item.alias] = ProjectionInfo(item, sources);
      }
      break;
    }
    case ir::LogicalPlanNodeType::kLetSemiApply:
      SetOverride(
          &overrides,
          static_cast<const ir::LetSemiApplyPlan &>(plan).ValueVariable(),
          SemanticType::kScalar, false);
      break;
    case ir::LogicalPlanNodeType::kRollUpApply:
      SetOverride(
          &overrides,
          static_cast<const ir::RollUpApplyPlan &>(plan).CollectionVariable(),
          SemanticType::kList, false);
      break;
    case ir::LogicalPlanNodeType::kAssertIsNode:
      for (const auto &variable :
           static_cast<const ir::AssertIsNodePlan &>(plan).Variables()) {
        SetOverride(&overrides, variable, SemanticType::kNode,
                    FindInfo(variable, sources).value_or(TypeInfo{}).nullable);
      }
      break;
    case ir::LogicalPlanNodeType::kCreateNode:
      SetOverride(&overrides,
                  static_cast<const ir::CreateNodePlan &>(plan).Node().variable,
                  SemanticType::kNode, false);
      break;
    case ir::LogicalPlanNodeType::kCreateRelationship: {
      const auto &relationship =
          static_cast<const ir::CreateRelationshipPlan &>(plan).Relationship();
      SetOverride(&overrides, relationship.left_node, SemanticType::kNode,
                  false);
      SetOverride(&overrides, relationship.variable,
                  SemanticType::kRelationship, false);
      SetOverride(&overrides, relationship.right_node, SemanticType::kNode,
                  false);
      break;
    }
    case ir::LogicalPlanNodeType::kMerge: {
      const auto &pattern =
          static_cast<const ir::MergePlan &>(plan).Merge().create_pattern;
      for (const auto &node : pattern.nodes) {
        SetOverride(&overrides, node.variable, SemanticType::kNode, false);
      }
      for (const auto &relationship : pattern.relationships) {
        SetOverride(&overrides, relationship.left_node, SemanticType::kNode,
                    false);
        SetOverride(&overrides, relationship.variable,
                    SemanticType::kRelationship, false);
        SetOverride(&overrides, relationship.right_node, SemanticType::kNode,
                    false);
      }
      break;
    }
    case ir::LogicalPlanNodeType::kDelete:
    case ir::LogicalPlanNodeType::kDetachDelete:
      if (!sources.empty()) {
        for (const auto &column : sources.front()->Columns()) {
          const Slot &slot = sources.front()->At(column);
          if (slot.kind != SlotKind::kReference) {
            overrides[column] = {.type = slot.type,
                                 .nullable = slot.nullable,
                                 .storage_kind = SlotKind::kReference};
          }
        }
      }
      break;
    case ir::LogicalPlanNodeType::kUnwind:
      SetOverride(&overrides, static_cast<const ir::UnwindPlan &>(plan).Alias(),
                  SemanticType::kUnknown, true);
      break;
    case ir::LogicalPlanNodeType::kProcedureCall: {
      const auto &call = static_cast<const ir::ProcedureCallPlan &>(plan);
      const ast::BuiltinProcedure *procedure =
          ast::FindBuiltinProcedure(call.ProcedureName());
      for (const auto &item : call.YieldItems()) {
        const std::string &field =
            item.result_field.has_value() ? *item.result_field : item.variable;
        const ast::BuiltinProcedureYield *yield =
            procedure == nullptr
                ? nullptr
                : ast::FindBuiltinProcedureYield(*procedure, field);
        SetOverride(&overrides, item.variable,
                    yield == nullptr ? SemanticType::kUnknown : yield->type,
                    true);
      }
      break;
    }
    default:
      break;
  }
  return overrides;
}

SlotConfigurationPtr MakeLayout(
    const std::vector<std::string> &columns,
    const std::vector<SlotConfigurationPtr> &sources,
    const TypeOverrides &overrides = {},
    const std::unordered_set<std::string> &force_nullable = {}) {
  std::vector<SlotDefinition> definitions;
  definitions.reserve(columns.size());
  for (const auto &column : columns) {
    TypeInfo info = FindInfo(column, sources).value_or(TypeInfo{});
    const auto override = overrides.find(column);
    if (override != overrides.end()) {
      info = override->second;
    }
    if (force_nullable.contains(column)) {
      info.nullable = true;
    }
    definitions.push_back({.name = column,
                           .type = info.type,
                           .nullable = info.nullable,
                           .storage_kind = info.storage_kind});
  }
  return std::make_shared<const SlotConfiguration>(std::move(definitions));
}

std::vector<SlotMapping> NamedMappings(
    const SlotConfiguration &source, const SlotConfiguration &target,
    const std::vector<std::pair<std::string, std::string>> &names) {
  std::vector<SlotMapping> mappings;
  mappings.reserve(names.size());
  for (const auto &[source_name, target_name] : names) {
    const Slot *source_slot = source.Find(source_name);
    const Slot *target_slot = target.Find(target_name);
    if (source_slot != nullptr && target_slot != nullptr) {
      mappings.push_back({.source_name = source_name,
                          .target_name = target_name,
                          .source = *source_slot,
                          .target = *target_slot});
    }
  }
  return mappings;
}

PhysicalExpression CopyPhysicalExpression(
    const ast::Expression *expression,
    const std::vector<ast::PrecomputedExpression> &precomputed) {
  CHECK(expression != nullptr, common::InvalidArgumentError,
        "physical expression is null");
  std::vector<PhysicalPrecomputedExpression> copied_precomputed;
  copied_precomputed.reserve(precomputed.size());
  for (const auto &entry : precomputed) {
    CHECK(entry.expression != nullptr, common::InvalidArgumentError,
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

PhysicalExpandDirection ToPhysicalExpandDirection(ir::Direction direction) {
  switch (direction) {
    case ir::Direction::kIncoming:
      return PhysicalExpandDirection::kIncoming;
    case ir::Direction::kOutgoing:
      return PhysicalExpandDirection::kOutgoing;
    case ir::Direction::kBoth:
      return PhysicalExpandDirection::kBoth;
  }
  THROW(common::InternalError, "unknown relationship direction");
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
  THROW(common::InternalError, "unknown expand direction");
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
    const std::vector<ir::LogicalProjectionItem> &items) {
  std::vector<PhysicalGroupingItem> copied;
  copied.reserve(items.size());
  for (const auto &item : items) {
    copied.push_back({.alias = item.alias,
                      .expression = CopyPhysicalExpression(
                          item.expression, item.precomputed_expressions)});
  }
  return copied;
}

std::vector<PhysicalAggregationItem> CopyPhysicalAggregationItems(
    const std::vector<ir::LogicalProjectionItem> &items) {
  std::vector<PhysicalAggregationItem> copied;
  copied.reserve(items.size());
  for (const auto &item : items) {
    copied.push_back({.alias = item.alias,
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
    empty_slots_ = MakeLayout({}, {});
    return PhysicalPlan(BuildNode(plan, empty_slots_));
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
    THROW(common::InternalError,
          "unsupported physical operator: " + std::string(plan.Name()));
  }

  std::unique_ptr<PhysicalPlanNode> BuildNode(
      const ir::LogicalPlan &plan, SlotConfigurationPtr argument_slots) {
    auto node = std::make_unique<PhysicalPlanNode>();
    node->id = next_id_++;
    node->logical = &plan;
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

    std::vector<SlotConfigurationPtr> sources;
    for (const auto &child : node->children) {
      sources.push_back(child->output_slots);
    }
    if (plan.ChildCount() == 0) {
      sources.push_back(node->argument_slots);
    }

    if (plan.Type() == ir::LogicalPlanNodeType::kUnion) {
      BuildUnionLayout(static_cast<const ir::UnionPlan &>(plan), node.get());
    } else {
      std::vector<std::string> columns = plan.OutputColumns();
      if (plan.ChildCount() == 0 &&
          plan.Type() != ir::LogicalPlanNodeType::kArgument) {
        columns = AppendUnique(node->argument_slots->Columns(), columns);
      }
      std::unordered_set<std::string> force_nullable;
      if (plan.Type() == ir::LogicalPlanNodeType::kOptionalApply ||
          plan.Type() == ir::LogicalPlanNodeType::kLeftOuterHashJoin) {
        const auto &lhs_columns = node->children[0]->output_slots->Columns();
        const std::unordered_set<std::string> lhs(lhs_columns.begin(),
                                                  lhs_columns.end());
        for (const auto &column : node->children[1]->output_slots->Columns()) {
          if (!lhs.contains(column)) {
            force_nullable.insert(column);
          }
        }
      }
      node->output_slots = MakeLayout(
          columns, sources, OutputOverrides(plan, sources), force_nullable);
      for (const auto &child : node->children) {
        node->child_mappings.push_back(
            ComputeSlotMappings(*child->output_slots, *node->output_slots));
      }
    }
    node->argument_mapping =
        ComputeSlotMappings(*node->argument_slots, *node->output_slots);
    SelectOperator(plan, node.get());
    BuildOperatorData(plan, node.get());
    return node;
  }

  void SelectOperator(const ir::LogicalPlan &plan, PhysicalPlanNode *node) {
    CHECK(node != nullptr, common::InternalError, "physical plan node is null");
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
      CHECK(node->children.size() == 1, common::InternalError,
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
      CHECK(node->children.size() == 1, common::InternalError,
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
      CHECK(node->children.size() == 1, common::InternalError,
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
      CHECK(node->children.size() == 1, common::InternalError,
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
    CHECK(node != nullptr, common::InternalError, "physical plan node is null");
    switch (node->kind) {
      case PhysicalOperatorKind::kArgument:
        node->data = ArgumentOp{};
        return;
      case PhysicalOperatorKind::kAllNodeScan: {
        const auto &scan = static_cast<const ir::AllNodeScanPlan &>(plan);
        node->data = AllNodeScanOp{.variable = scan.Variable()};
        return;
      }
      case PhysicalOperatorKind::kNodeByLabelScan: {
        const auto &scan = static_cast<const ir::NodeByLabelScanPlan &>(plan);
        node->data = NodeByLabelScanOp{.variable = scan.Variable(),
                                       .labels = scan.Labels()};
        return;
      }
      case PhysicalOperatorKind::kNodeIndexSeek: {
        const auto &seek = static_cast<const ir::NodeIndexSeekPlan &>(plan);
        node->data = NodeIndexSeekOp{
            .variable = seek.Variable(),
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
            .labels = seek.Labels(),
            .property_key = seek.PropertyKey(),
            .predicates = CopyPhysicalExpressions(seek.Predicates())};
        return;
      }
      case PhysicalOperatorKind::kRelationshipTypeScan: {
        const auto &scan =
            static_cast<const ir::RelationshipTypeScanPlan &>(plan);
        node->data = RelationshipTypeScanOp{
            .pattern = {
                .from_node = scan.FromNode(),
                .relationship = scan.Relationship(),
                .to_node = scan.ToNode(),
                .direction = ToPhysicalExpandDirection(scan.Direction()),
                .types = scan.Types()}};
        return;
      }
      case PhysicalOperatorKind::kRelationshipIndexSeek: {
        const auto &seek =
            static_cast<const ir::RelationshipIndexSeekPlan &>(plan);
        node->data = RelationshipIndexSeekOp{
            .pattern = {.from_node = seek.FromNode(),
                        .relationship = seek.Relationship(),
                        .to_node = seek.ToNode(),
                        .direction =
                            ToPhysicalExpandDirection(seek.Direction()),
                        .types = seek.Types()},
            .property_key = seek.PropertyKey(),
            .value = CopyPhysicalExpression(seek.ValueExpression(), {}),
            .unique = seek.Unique()};
        return;
      }
      case PhysicalOperatorKind::kRelationshipIndexRangeSeek: {
        const auto &seek =
            static_cast<const ir::RelationshipIndexRangeSeekPlan &>(plan);
        node->data = RelationshipIndexRangeSeekOp{
            .pattern = {.from_node = seek.FromNode(),
                        .relationship = seek.Relationship(),
                        .to_node = seek.ToNode(),
                        .direction =
                            ToPhysicalExpandDirection(seek.Direction()),
                        .types = seek.Types()},
            .property_key = seek.PropertyKey(),
            .predicates = CopyPhysicalExpressions(seek.Predicates())};
        return;
      }
      case PhysicalOperatorKind::kNodeByIdSeek: {
        const auto &seek = static_cast<const ir::NodeByIdSeekPlan &>(plan);
        node->data =
            NodeByIdSeekOp{.variable = seek.Variable(),
                           .ids = CopyPhysicalExpression(seek.Ids(), {}),
                           .many = seek.Many()};
        return;
      }
      case PhysicalOperatorKind::kRelationshipByIdSeek: {
        const auto &seek =
            static_cast<const ir::RelationshipByIdSeekPlan &>(plan);
        node->data = RelationshipByIdSeekOp{
            .pattern = CopyPhysicalRelationshipPattern(seek.Pattern()),
            .ids = CopyPhysicalExpression(seek.Ids(), {}),
            .many = seek.Many()};
        return;
      }
      case PhysicalOperatorKind::kExpand: {
        const auto &expand = static_cast<const ir::ExpandPlan &>(plan);
        node->data = ExpandOp{
            .pattern = {
                .from_node = expand.FromNode(),
                .relationship = expand.Relationship(),
                .to_node = expand.ToNode(),
                .direction = ToPhysicalExpandDirection(expand.Direction()),
                .types = expand.Types()}};
        return;
      }
      case PhysicalOperatorKind::kExpandInto: {
        const auto &expand = static_cast<const ir::ExpandIntoPlan &>(plan);
        node->data = ExpandIntoOp{
            .pattern = {
                .from_node = expand.FromNode(),
                .relationship = expand.Relationship(),
                .to_node = expand.ToNode(),
                .direction = ToPhysicalExpandDirection(expand.Direction()),
                .types = expand.Types()}};
        return;
      }
      case PhysicalOperatorKind::kVarExpand: {
        const auto &expand = static_cast<const ir::VarExpandPlan &>(plan);
        node->data = VarExpandOp{
            .pattern = {.from_node = expand.FromNode(),
                        .relationship = expand.Relationship(),
                        .to_node = expand.ToNode(),
                        .direction =
                            ToPhysicalExpandDirection(expand.Direction()),
                        .types = expand.Types()},
            .length = {.variable = true,
                       .min = expand.Length().min,
                       .max = expand.Length().max}};
        return;
      }
      case PhysicalOperatorKind::kPruningVarExpand: {
        const auto &expand =
            static_cast<const ir::PruningVarExpandPlan &>(plan);
        const ir::PatternRelationship &pattern = expand.Pattern();
        node->data = PruningVarExpandOp{
            .pattern = CopyPhysicalRelationshipPattern(pattern),
            .length = {.variable = pattern.length.variable,
                       .min = pattern.length.min,
                       .max = pattern.length.max}};
        return;
      }
      case PhysicalOperatorKind::kOptionalExpand: {
        const auto &expand = static_cast<const ir::OptionalExpandPlan &>(plan);
        node->data = OptionalExpandOp{
            .pattern = CopyPhysicalRelationshipPattern(expand.Pattern()),
            .predicates = CopyPhysicalExpressions(expand.Predicates())};
        return;
      }
      case PhysicalOperatorKind::kProjectEndpoints: {
        const auto &project =
            static_cast<const ir::ProjectEndpointsPlan &>(plan);
        const ir::PatternRelationship &pattern = project.Pattern();
        node->data = ProjectEndpointsOp{
            .pattern = CopyPhysicalRelationshipPattern(pattern),
            .length = {.variable = pattern.length.variable,
                       .min = pattern.length.min,
                       .max = pattern.length.max}};
        return;
      }
      case PhysicalOperatorKind::kPathBuild: {
        const auto &build = static_cast<const ir::PathBuildPlan &>(plan);
        node->data =
            PathBuildOp{.path = {.variable = build.Path().variable,
                                 .nodes = build.Path().nodes,
                                 .relationships = build.Path().relationships}};
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
          PhysicalProjectionItem physical_item{.alias = item.alias,
                                               .passthrough = item.passthrough};
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
        node->data = HashDistinctOp{.grouping_items = CopyPhysicalGroupingItems(
                                        distinct.GroupingItems())};
        return;
      }
      case PhysicalOperatorKind::kOrderedDistinct: {
        const auto &distinct = static_cast<const ir::DistinctPlan &>(plan);
        node->data =
            OrderedDistinctOp{.grouping_items = CopyPhysicalGroupingItems(
                                  distinct.GroupingItems())};
        return;
      }
      case PhysicalOperatorKind::kHashAggregation: {
        const auto &aggregation =
            static_cast<const ir::AggregationPlan &>(plan);
        node->data = HashAggregationOp{
            .grouping_items =
                CopyPhysicalGroupingItems(aggregation.GroupingItems()),
            .aggregation_items =
                CopyPhysicalAggregationItems(aggregation.AggregationItems())};
        return;
      }
      case PhysicalOperatorKind::kOrderedAggregation: {
        const auto &aggregation =
            static_cast<const ir::AggregationPlan &>(plan);
        node->data = OrderedAggregationOp{
            .grouping_items =
                CopyPhysicalGroupingItems(aggregation.GroupingItems()),
            .aggregation_items =
                CopyPhysicalAggregationItems(aggregation.AggregationItems())};
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
        node->data =
            LimitOp{.count = CopyPhysicalExpression(
                        limit.Limit(), limit.PrecomputedExpressions()),
                    .exhaust_child = PlanContainsWrites(plan.Child(0))};
        return;
      }
      case PhysicalOperatorKind::kProduceResults:
        node->data = ProduceResultsOp{.columns = plan.OutputColumns()};
        return;
      case PhysicalOperatorKind::kCartesianProduct:
        node->data = CartesianProductOp{};
        return;
      case PhysicalOperatorKind::kNodeHashJoin: {
        const auto &join = static_cast<const ir::NodeHashJoinPlan &>(plan);
        NodeHashJoinOp data{.join_keys = join.JoinKeys()};
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
        node->data = LeftOuterHashJoinOp{.join_keys = join.JoinKeys()};
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
        node->data = OptionalApplyOp{};
        return;
      default:
        return;
    }
  }

  void BuildUnionLayout(const ir::UnionPlan &plan, PhysicalPlanNode *node) {
    CHECK(node != nullptr && node->children.size() == 2, common::InternalError,
          "union physical node is incomplete");
    TypeOverrides overrides;
    std::vector<std::pair<std::string, std::string>> left_names;
    std::vector<std::pair<std::string, std::string>> right_names;
    for (const auto &mapping : plan.Mappings()) {
      const Slot &left =
          node->children[0]->output_slots->At(mapping.lhs_variable);
      const Slot &right =
          node->children[1]->output_slots->At(mapping.rhs_variable);
      overrides[mapping.output_variable] =
          MergeInfo(InfoForSlot(left), InfoForSlot(right));
      left_names.emplace_back(mapping.lhs_variable, mapping.output_variable);
      right_names.emplace_back(mapping.rhs_variable, mapping.output_variable);
    }
    node->output_slots = MakeLayout(plan.OutputColumns(), {}, overrides);
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
  THROW(common::InternalError, "unknown physical operator kind");
}

std::string_view ToString(PhysicalSortDirection direction) {
  switch (direction) {
    case PhysicalSortDirection::kAscending:
      return "ASC";
    case PhysicalSortDirection::kDescending:
      return "DESC";
  }
  THROW(common::InternalError, "unknown physical sort direction");
}

PhysicalPlan::PhysicalPlan(std::unique_ptr<PhysicalPlanNode> root)
    : root_(std::move(root)) {
  CHECK(root_ != nullptr, common::InvalidArgumentError,
        "physical plan root is null");
  Index(*root_);
}

const PhysicalPlanNode &PhysicalPlan::Root() const { return *root_; }

const PhysicalPlanNode &PhysicalPlan::NodeFor(
    const ir::LogicalPlan &logical) const {
  const auto found = nodes_.find(&logical);
  CHECK(found != nodes_.end(), common::InvalidArgumentError,
        "logical plan is not part of the physical plan");
  return *found->second;
}

void PhysicalPlan::Index(const PhysicalPlanNode &node) {
  nodes_.emplace(node.logical, &node);
  for (const auto &child : node.children) {
    Index(*child);
  }
}

PhysicalPlan CreatePhysicalPlan(const ir::LogicalPlan &plan) {
  return PhysicalPlanBuilder().Build(plan);
}

bool PlanContainsWrites(const ir::LogicalPlan &plan) {
  switch (plan.Type()) {
    case ir::LogicalPlanNodeType::kCreateNode:
    case ir::LogicalPlanNodeType::kCreateRelationship:
    case ir::LogicalPlanNodeType::kMerge:
    case ir::LogicalPlanNodeType::kSetProperty:
    case ir::LogicalPlanNodeType::kSetProperties:
    case ir::LogicalPlanNodeType::kSetLabels:
    case ir::LogicalPlanNodeType::kRemoveProperty:
    case ir::LogicalPlanNodeType::kRemoveLabels:
    case ir::LogicalPlanNodeType::kDelete:
    case ir::LogicalPlanNodeType::kDetachDelete:
      return true;
    case ir::LogicalPlanNodeType::kProcedureCall:
      if (!static_cast<const ir::ProcedureCallPlan &>(plan).ReadOnly()) {
        return true;
      }
      break;
    default:
      break;
  }
  for (const auto &child : plan.Children()) {
    if (child != nullptr && PlanContainsWrites(*child)) {
      return true;
    }
  }
  return false;
}

}  // namespace rg
