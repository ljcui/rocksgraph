#include "runtime/physical_plan.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "ast/ast_equal.h"
#include "ast/ast_node.h"
#include "ast/builtin_function.h"
#include "ast/builtin_procedure.h"
#include "common/exception.h"
#include "planner/order_property.h"

namespace rg {
namespace {

using SemanticType = ast::SemanticVariableType;

struct TypeInfo {
  SemanticType type = SemanticType::kUnknown;
  bool nullable = true;
  std::optional<SlotKind> storage_kind;
};

using TypeOverrides = std::unordered_map<std::string, TypeInfo>;

std::size_t CommonOrderingPrefix(
    const std::vector<ir::LogicalSortItem> &provided,
    const std::vector<ir::LogicalSortItem> &required) {
  const std::size_t limit = std::min(provided.size(), required.size());
  std::size_t prefix = 0;
  while (prefix < limit && provided[prefix].expression != nullptr &&
         required[prefix].expression != nullptr &&
         provided[prefix].direction == required[prefix].direction &&
         ast::ASTEqual::Equal(provided[prefix].expression,
                              required[prefix].expression)) {
    ++prefix;
  }
  return prefix;
}

ir::LogicalSortItem AliasSortItem(std::string alias,
                                  ir::LogicalOrderDirection direction) {
  auto variable = std::make_shared<ast::Variable>();
  variable->name = std::move(alias);
  const ast::Expression *expression = variable.get();
  return {.expression = expression,
          .owned_expression = std::move(variable),
          .direction = direction};
}

std::optional<std::vector<ir::LogicalSortItem>> OrderedGroupingOutput(
    const std::vector<ir::LogicalSortItem> &input_order,
    const std::vector<ir::LogicalProjectionItem> &grouping_items) {
  if (grouping_items.empty() || input_order.size() < grouping_items.size()) {
    return std::nullopt;
  }

  std::vector<bool> matched(grouping_items.size(), false);
  std::vector<ir::LogicalSortItem> output_order;
  output_order.reserve(grouping_items.size());
  for (std::size_t order_index = 0; order_index < grouping_items.size();
       ++order_index) {
    const ir::LogicalSortItem &order = input_order[order_index];
    if (order.expression == nullptr) {
      return std::nullopt;
    }
    std::optional<std::size_t> grouping_index;
    for (std::size_t index = 0; index < grouping_items.size(); ++index) {
      const ir::LogicalProjectionItem &grouping = grouping_items[index];
      if (!matched[index] && grouping.expression != nullptr &&
          ast::ASTEqual::Equal(order.expression, grouping.expression)) {
        grouping_index = index;
        break;
      }
    }
    if (!grouping_index.has_value()) {
      return std::nullopt;
    }
    matched[*grouping_index] = true;
    output_order.push_back(
        AliasSortItem(grouping_items[*grouping_index].alias, order.direction));
  }
  return output_order;
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

class PhysicalPlanBuilder final {
 public:
  PhysicalPlan Build(const ir::LogicalPlan &plan) {
    empty_slots_ = MakeLayout({}, {});
    return PhysicalPlan(BuildNode(plan, empty_slots_));
  }

 private:
  PhysicalExecutionType SelectExecutionKind(const ir::LogicalPlan &plan,
                                            const PhysicalPlanNode &node) {
    if (plan.ChildCount() == 0) {
      switch (plan.Type()) {
        case ir::LogicalPlanNodeType::kArgument:
        case ir::LogicalPlanNodeType::kAllNodeScan:
        case ir::LogicalPlanNodeType::kNodeByLabelScan:
        case ir::LogicalPlanNodeType::kNodeIndexSeek:
        case ir::LogicalPlanNodeType::kNodeIndexRangeSeek:
        case ir::LogicalPlanNodeType::kRelationshipTypeScan:
        case ir::LogicalPlanNodeType::kRelationshipIndexSeek:
        case ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek:
        case ir::LogicalPlanNodeType::kNodeByIdSeek:
        case ir::LogicalPlanNodeType::kRelationshipByIdSeek:
          return PhysicalExecutionType::kLeaf;
        default:
          THROW(common::InternalError,
                "unsupported physical leaf kind: " + std::string(plan.Name()));
      }
    }
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kFilter:
      case ir::LogicalPlanNodeType::kProjection:
      case ir::LogicalPlanNodeType::kSkip:
      case ir::LogicalPlanNodeType::kProduceResults:
      case ir::LogicalPlanNodeType::kAssertIsNode:
      case ir::LogicalPlanNodeType::kExpand:
      case ir::LogicalPlanNodeType::kExpandInto:
      case ir::LogicalPlanNodeType::kPathBuild:
      case ir::LogicalPlanNodeType::kProcedureCall:
      case ir::LogicalPlanNodeType::kUnwind:
      case ir::LogicalPlanNodeType::kSetProperty:
      case ir::LogicalPlanNodeType::kSetProperties:
      case ir::LogicalPlanNodeType::kSetLabels:
      case ir::LogicalPlanNodeType::kRemoveProperty:
      case ir::LogicalPlanNodeType::kRemoveLabels:
      case ir::LogicalPlanNodeType::kCreateNode:
      case ir::LogicalPlanNodeType::kCreateRelationship:
        return PhysicalExecutionType::kStreamingUnary;
      case ir::LogicalPlanNodeType::kLimit:
        return PlanContainsWrites(plan.Child(0))
                   ? PhysicalExecutionType::kBlockingUnary
                   : PhysicalExecutionType::kStreamingUnary;
      case ir::LogicalPlanNodeType::kWriteBarrier:
      case ir::LogicalPlanNodeType::kDelete:
      case ir::LogicalPlanNodeType::kDetachDelete:
        return PhysicalExecutionType::kBlockingUnary;
      case ir::LogicalPlanNodeType::kSort:
        return node.type == PhysicalOperatorType::kPartialSort
                   ? PhysicalExecutionType::kPartialSort
                   : PhysicalExecutionType::kBlockingUnary;
      case ir::LogicalPlanNodeType::kTopN:
        return node.type == PhysicalOperatorType::kPartialTopN
                   ? PhysicalExecutionType::kPartialTopN
                   : PhysicalExecutionType::kTopN;
      case ir::LogicalPlanNodeType::kDistinct:
        return node.type == PhysicalOperatorType::kOrderedDistinct
                   ? PhysicalExecutionType::kOrderedDistinct
                   : PhysicalExecutionType::kBlockingUnary;
      case ir::LogicalPlanNodeType::kAggregation:
        return node.type == PhysicalOperatorType::kOrderedAggregation
                   ? PhysicalExecutionType::kOrderedAggregation
                   : PhysicalExecutionType::kBlockingUnary;
      case ir::LogicalPlanNodeType::kVarExpand:
        return PhysicalExecutionType::kVarExpand;
      case ir::LogicalPlanNodeType::kPruningVarExpand:
        return PhysicalExecutionType::kPruningVarExpand;
      case ir::LogicalPlanNodeType::kProjectEndpoints:
      case ir::LogicalPlanNodeType::kOptionalExpand:
        return PhysicalExecutionType::kPattern;
      case ir::LogicalPlanNodeType::kApply:
      case ir::LogicalPlanNodeType::kSemiApply:
      case ir::LogicalPlanNodeType::kAntiSemiApply:
      case ir::LogicalPlanNodeType::kLetSemiApply:
      case ir::LogicalPlanNodeType::kSelectOrSemiApply:
      case ir::LogicalPlanNodeType::kRollUpApply:
      case ir::LogicalPlanNodeType::kOptionalApply:
        return PhysicalExecutionType::kApply;
      case ir::LogicalPlanNodeType::kMerge:
        return PhysicalExecutionType::kMerge;
      case ir::LogicalPlanNodeType::kUnion:
        return PhysicalExecutionType::kUnion;
      case ir::LogicalPlanNodeType::kValueHashJoin:
        return PhysicalExecutionType::kValueHashJoin;
      case ir::LogicalPlanNodeType::kLeftOuterHashJoin:
        return PhysicalExecutionType::kLeftOuterHashJoin;
      case ir::LogicalPlanNodeType::kCartesianProduct:
      case ir::LogicalPlanNodeType::kNodeHashJoin:
      case ir::LogicalPlanNodeType::kPredicateJoin:
        return PhysicalExecutionType::kBlockingBinary;
      default:
        THROW(common::InternalError, "unsupported physical execution kind: " +
                                         std::string(plan.Name()));
    }
  }

  std::unique_ptr<PhysicalPlanNode> BuildNode(
      const ir::LogicalPlan &plan, SlotConfigurationPtr argument_slots) {
    auto node = std::make_unique<PhysicalPlanNode>();
    node->id = next_id_++;
    node->logical = &plan;
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
    if (plan.Type() == ir::LogicalPlanNodeType::kValueHashJoin) {
      const auto &left_rows = plan.Child(0).EstimatedRows();
      const auto &right_rows = plan.Child(1).EstimatedRows();
      if (left_rows.has_value() && right_rows.has_value() &&
          *left_rows < *right_rows) {
        node->value_hash_join_build_child = 0;
      }
    }
    SelectOperator(plan, node.get());
    return node;
  }

  void SelectOperator(const ir::LogicalPlan &plan, PhysicalPlanNode *node) {
    CHECK(node != nullptr, common::InternalError, "physical plan node is null");
    node->provided_order = plan.OrderingTrait();
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
          node->provided_order = ir::ProjectOrdering(
              node->children[0]->provided_order,
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
      node->partial_sort_prefix =
          CommonOrderingPrefix(node->children[0]->provided_order, sort.Items());
      node->type = node->partial_sort_prefix == 0
                       ? PhysicalOperatorType::kFullSort
                       : PhysicalOperatorType::kPartialSort;
      node->execution_kind = node->type == PhysicalOperatorType::kPartialSort
                                 ? PhysicalExecutionType::kPartialSort
                                 : PhysicalExecutionType::kBlockingUnary;
      node->provided_order = sort.Items();
      return;
    }

    if (plan.Type() == ir::LogicalPlanNodeType::kTopN) {
      CHECK(node->children.size() == 1, common::InternalError,
            "Top-N physical node must have one child");
      const auto &top_n = static_cast<const ir::TopNPlan &>(plan);
      node->top_n = &top_n;
      node->partial_top_n_prefix = CommonOrderingPrefix(
          node->children[0]->provided_order, top_n.Items());
      node->type = node->partial_top_n_prefix == 0
                       ? PhysicalOperatorType::kTopN
                       : PhysicalOperatorType::kPartialTopN;
      node->execution_kind = node->type == PhysicalOperatorType::kPartialTopN
                                 ? PhysicalExecutionType::kPartialTopN
                                 : PhysicalExecutionType::kTopN;
      node->provided_order = top_n.Items();
      return;
    }

    if (plan.Type() == ir::LogicalPlanNodeType::kDistinct) {
      CHECK(node->children.size() == 1, common::InternalError,
            "Distinct physical node must have one child");
      const auto &distinct = static_cast<const ir::DistinctPlan &>(plan);
      const auto output_order = OrderedGroupingOutput(
          node->children[0]->provided_order, distinct.GroupingItems());
      node->type = output_order.has_value()
                       ? PhysicalOperatorType::kOrderedDistinct
                       : PhysicalOperatorType::kHashDistinct;
      node->execution_kind = output_order.has_value()
                                 ? PhysicalExecutionType::kOrderedDistinct
                                 : PhysicalExecutionType::kBlockingUnary;
      node->provided_order =
          output_order.value_or(std::vector<ir::LogicalSortItem>{});
      return;
    }

    if (plan.Type() == ir::LogicalPlanNodeType::kAggregation) {
      CHECK(node->children.size() == 1, common::InternalError,
            "Aggregation physical node must have one child");
      const auto &aggregation = static_cast<const ir::AggregationPlan &>(plan);
      const auto output_order = OrderedGroupingOutput(
          node->children[0]->provided_order, aggregation.GroupingItems());
      node->type = output_order.has_value()
                       ? PhysicalOperatorType::kOrderedAggregation
                       : PhysicalOperatorType::kHashAggregation;
      node->execution_kind = output_order.has_value()
                                 ? PhysicalExecutionType::kOrderedAggregation
                                 : PhysicalExecutionType::kBlockingUnary;
      node->provided_order =
          output_order.value_or(std::vector<ir::LogicalSortItem>{});
      return;
    }
    node->execution_kind = SelectExecutionKind(plan, *node);
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

std::string_view ToString(PhysicalOperatorType type) {
  switch (type) {
    case PhysicalOperatorType::kLogical:
      return "Logical";
    case PhysicalOperatorType::kFullSort:
      return "FullSort";
    case PhysicalOperatorType::kPartialSort:
      return "PartialSort";
    case PhysicalOperatorType::kHashDistinct:
      return "HashDistinct";
    case PhysicalOperatorType::kOrderedDistinct:
      return "OrderedDistinct";
    case PhysicalOperatorType::kHashAggregation:
      return "HashAggregation";
    case PhysicalOperatorType::kOrderedAggregation:
      return "OrderedAggregation";
    case PhysicalOperatorType::kTopN:
      return "TopN";
    case PhysicalOperatorType::kPartialTopN:
      return "PartialTopN";
  }
  THROW(common::InternalError, "unknown physical operator type");
}

std::string_view ToString(PhysicalExecutionType type) {
  switch (type) {
    case PhysicalExecutionType::kUnknown:
      return "Unknown";
    case PhysicalExecutionType::kLeaf:
      return "Leaf";
    case PhysicalExecutionType::kStreamingUnary:
      return "StreamingUnary";
    case PhysicalExecutionType::kBlockingUnary:
      return "BlockingUnary";
    case PhysicalExecutionType::kPattern:
      return "Pattern";
    case PhysicalExecutionType::kVarExpand:
      return "VarExpand";
    case PhysicalExecutionType::kPruningVarExpand:
      return "PruningVarExpand";
    case PhysicalExecutionType::kOrderedDistinct:
      return "OrderedDistinct";
    case PhysicalExecutionType::kOrderedAggregation:
      return "OrderedAggregation";
    case PhysicalExecutionType::kPartialSort:
      return "PartialSort";
    case PhysicalExecutionType::kTopN:
      return "TopN";
    case PhysicalExecutionType::kPartialTopN:
      return "PartialTopN";
    case PhysicalExecutionType::kLeftOuterHashJoin:
      return "LeftOuterHashJoin";
    case PhysicalExecutionType::kBlockingBinary:
      return "BlockingBinary";
    case PhysicalExecutionType::kValueHashJoin:
      return "ValueHashJoin";
    case PhysicalExecutionType::kUnion:
      return "Union";
    case PhysicalExecutionType::kApply:
      return "Apply";
    case PhysicalExecutionType::kMerge:
      return "Merge";
  }
  THROW(common::InternalError, "unknown physical execution type");
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
