#include "ir/logical_plan.h"

#include <array>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "ast/expression_to_string.h"
#include "common/exception.h"

namespace ir {
namespace {

inline constexpr auto kLogicalPlanNodeTypeNames = std::array{
    std::string_view{"Argument"},
    std::string_view{"AllNodeScan"},
    std::string_view{"NodeByLabelScan"},
    std::string_view{"Expand"},
    std::string_view{"ExpandInto"},
    std::string_view{"VarExpand"},
    std::string_view{"PathBuild"},
    std::string_view{"Filter"},
    std::string_view{"Projection"},
    std::string_view{"Distinct"},
    std::string_view{"Aggregation"},
    std::string_view{"Sort"},
    std::string_view{"Skip"},
    std::string_view{"Limit"},
    std::string_view{"ProduceResults"},
    std::string_view{"CartesianProduct"},
    std::string_view{"NodeHashJoin"},
    std::string_view{"Apply"},
    std::string_view{"SemiApply"},
    std::string_view{"LetSemiApply"},
    std::string_view{"RollUpApply"},
    std::string_view{"OptionalApply"},
    std::string_view{"AssertIsNode"},
    std::string_view{"Unwind"},
    std::string_view{"Union"},
};

static_assert(kLogicalPlanNodeTypeNames.size() ==
              static_cast<std::size_t>(LogicalPlanNodeType::kUnion) + 1);

void AddSymbol(std::unordered_set<std::string> *symbols,
               std::string_view symbol) {
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
  if (!symbol.empty()) {
    symbols->emplace(symbol);
  }
}

void AddColumn(std::vector<std::string> *columns, std::string_view column) {
  CHECK(columns != nullptr, common::InternalError, "column list is null");
  if (column.empty()) {
    return;
  }
  for (const auto &existing : *columns) {
    if (existing == column) {
      return;
    }
  }
  columns->emplace_back(column);
}

std::unordered_set<std::string> SymbolsFromColumns(
    const std::vector<std::string> &columns) {
  std::unordered_set<std::string> symbols;
  for (const auto &column : columns) {
    AddSymbol(&symbols, column);
  }
  return symbols;
}

std::vector<LogicalPlanPtr> UnaryChildren(LogicalPlanPtr source,
                                          std::string_view node_name) {
  CHECK(source != nullptr, common::InvalidArgumentError,
        std::string(node_name) + " source is null");
  std::vector<LogicalPlanPtr> children;
  children.push_back(std::move(source));
  return children;
}

std::vector<LogicalPlanPtr> BinaryChildren(LogicalPlanPtr left,
                                           LogicalPlanPtr right,
                                           std::string_view node_name) {
  CHECK(left != nullptr, common::InvalidArgumentError,
        std::string(node_name) + " left input is null");
  CHECK(right != nullptr, common::InvalidArgumentError,
        std::string(node_name) + " right input is null");
  std::vector<LogicalPlanPtr> children;
  children.push_back(std::move(left));
  children.push_back(std::move(right));
  return children;
}

std::unordered_set<std::string> UnionSolvedSymbols(const LogicalPlan &left,
                                                   const LogicalPlan &right) {
  std::unordered_set<std::string> symbols = left.SolvedSymbols();
  for (const auto &symbol : right.SolvedSymbols()) {
    AddSymbol(&symbols, symbol);
  }
  return symbols;
}

std::vector<std::string> UnionOutputColumns(const LogicalPlan &left,
                                            const LogicalPlan &right) {
  std::vector<std::string> columns = left.OutputColumns();
  for (const auto &column : right.OutputColumns()) {
    AddColumn(&columns, column);
  }
  return columns;
}

std::string Join(const std::vector<std::string> &values,
                 std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out.append(separator);
    }
    out.append(values[i]);
  }
  return out;
}

std::vector<std::string> ProjectionItemAliases(
    const std::vector<LogicalProjectionItem> &items) {
  std::vector<std::string> aliases;
  aliases.reserve(items.size());
  for (const auto &item : items) {
    aliases.push_back(item.alias);
  }
  return aliases;
}

std::vector<std::string> ProjectionItemAliases(
    const std::vector<LogicalProjectionItem> &lhs,
    const std::vector<LogicalProjectionItem> &rhs) {
  std::vector<std::string> aliases = ProjectionItemAliases(lhs);
  aliases.reserve(lhs.size() + rhs.size());
  for (const auto &item : rhs) {
    aliases.push_back(item.alias);
  }
  return aliases;
}

std::vector<std::string> UnionOutputVariables(
    const std::vector<LogicalUnionMapping> &mappings) {
  std::vector<std::string> columns;
  columns.reserve(mappings.size());
  for (const auto &mapping : mappings) {
    columns.push_back(mapping.output_variable);
  }
  return columns;
}

std::string ExpandArrow(ExpandDirection direction, bool left) {
  switch (direction) {
    case ExpandDirection::kIncoming:
      return left ? "<-" : "-";
    case ExpandDirection::kOutgoing:
      return left ? "-" : "->";
    case ExpandDirection::kBoth:
      return "-";
  }
  THROW(common::InternalError, "unknown expand direction");
}

std::string RelationshipDetails(std::string_view from_node,
                                std::string_view relationship,
                                std::string_view to_node,
                                ExpandDirection direction,
                                const std::vector<std::string> &types) {
  std::ostringstream out;
  out << "(" << from_node << ")" << ExpandArrow(direction, true) << "["
      << relationship;
  if (!types.empty()) {
    out << ":" << Join(types, "|");
  }
  out << "]" << ExpandArrow(direction, false) << "(" << to_node << ")";
  return out.str();
}

std::string VariableLengthDetails(const LogicalVariableLength &length) {
  if (!length.min.has_value() && !length.max.has_value()) {
    return "*";
  }
  std::string out = "*";
  if (length.min.has_value()) {
    out.append(std::to_string(*length.min));
  }
  out.append("..");
  if (length.max.has_value()) {
    out.append(std::to_string(*length.max));
  }
  return out;
}

std::string VarRelationshipDetails(std::string_view from_node,
                                   std::string_view relationship,
                                   std::string_view to_node,
                                   ExpandDirection direction,
                                   const std::vector<std::string> &types,
                                   const LogicalVariableLength &length) {
  std::ostringstream out;
  out << "(" << from_node << ")" << ExpandArrow(direction, true) << "["
      << relationship;
  if (!types.empty()) {
    out << ":" << Join(types, "|");
  }
  out << VariableLengthDetails(length) << "]" << ExpandArrow(direction, false)
      << "(" << to_node << ")";
  return out.str();
}

}  // namespace

std::string_view ToString(LogicalPlanNodeType type) {
  const auto index = static_cast<std::size_t>(type);
  if (index >= kLogicalPlanNodeTypeNames.size()) {
    return "Unknown";
  }
  return kLogicalPlanNodeTypeNames[index];
}

std::string_view ToString(ExpandDirection direction) {
  switch (direction) {
    case ExpandDirection::kIncoming:
      return "incoming";
    case ExpandDirection::kOutgoing:
      return "outgoing";
    case ExpandDirection::kBoth:
      return "both";
  }
  THROW(common::InternalError, "unknown expand direction");
}

std::string_view ToString(LogicalOrderDirection direction) {
  switch (direction) {
    case LogicalOrderDirection::kAscending:
      return "ASC";
    case LogicalOrderDirection::kDescending:
      return "DESC";
  }
  THROW(common::InternalError, "unknown order direction");
}

LogicalPlan::LogicalPlan(LogicalPlanNodeType type,
                         std::vector<LogicalPlanPtr> children)
    : type_(type), children_(std::move(children)) {}

LogicalPlan::~LogicalPlan() = default;

std::string LogicalPlan::Details() const { return {}; }

const LogicalPlan &LogicalPlan::Child(std::size_t index) const {
  CHECK(index < children_.size(), common::InvalidArgumentError,
        "logical plan child index out of range");
  return *children_[index];
}

LogicalPlan &LogicalPlan::Child(std::size_t index) {
  CHECK(index < children_.size(), common::InvalidArgumentError,
        "logical plan child index out of range");
  return *children_[index];
}

void LogicalPlan::SetSolvedSymbols(std::unordered_set<std::string> symbols) {
  solved_symbols_ = std::move(symbols);
}

void LogicalPlan::SetOutputColumns(std::vector<std::string> columns) {
  output_columns_.clear();
  for (const auto &column : columns) {
    AddColumn(&output_columns_, column);
  }
}

void LogicalPlan::AddSolvedSymbol(std::string_view symbol) {
  AddSymbol(&solved_symbols_, symbol);
}

void LogicalPlan::AddOutputColumn(std::string_view column) {
  AddColumn(&output_columns_, column);
}

ArgumentPlan::ArgumentPlan(std::vector<std::string> symbols)
    : LogicalPlan(LogicalPlanNodeType::kArgument) {
  SetOutputColumns(std::move(symbols));
  SetSolvedSymbols(SymbolsFromColumns(OutputColumns()));
}

std::string ArgumentPlan::Details() const {
  return Join(OutputColumns(), ", ");
}

AllNodeScanPlan::AllNodeScanPlan(std::string variable)
    : LogicalPlan(LogicalPlanNodeType::kAllNodeScan),
      variable_(std::move(variable)) {
  AddOutputColumn(variable_);
  AddSolvedSymbol(variable_);
}

std::string AllNodeScanPlan::Details() const { return variable_; }

NodeByLabelScanPlan::NodeByLabelScanPlan(std::string variable,
                                         std::string label)
    : LogicalPlan(LogicalPlanNodeType::kNodeByLabelScan),
      variable_(std::move(variable)),
      label_(std::move(label)) {
  AddOutputColumn(variable_);
  AddSolvedSymbol(variable_);
}

std::string NodeByLabelScanPlan::Details() const {
  if (label_.empty()) {
    return variable_;
  }
  return variable_ + ":" + label_;
}

ExpandPlan::ExpandPlan(LogicalPlanPtr source, std::string from_node,
                       std::string relationship, std::string to_node,
                       ExpandDirection direction,
                       std::vector<std::string> types)
    : LogicalPlan(LogicalPlanNodeType::kExpand,
                  UnaryChildren(std::move(source), "Expand")),
      from_node_(std::move(from_node)),
      relationship_(std::move(relationship)),
      to_node_(std::move(to_node)),
      direction_(direction),
      types_(std::move(types)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
  AddSolvedSymbol(from_node_);
  AddSolvedSymbol(relationship_);
  AddSolvedSymbol(to_node_);
  AddOutputColumn(from_node_);
  AddOutputColumn(relationship_);
  AddOutputColumn(to_node_);
}

std::string ExpandPlan::Details() const {
  return RelationshipDetails(from_node_, relationship_, to_node_, direction_,
                             types_);
}

ExpandIntoPlan::ExpandIntoPlan(LogicalPlanPtr source, std::string from_node,
                               std::string relationship, std::string to_node,
                               ExpandDirection direction,
                               std::vector<std::string> types)
    : LogicalPlan(LogicalPlanNodeType::kExpandInto,
                  UnaryChildren(std::move(source), "ExpandInto")),
      from_node_(std::move(from_node)),
      relationship_(std::move(relationship)),
      to_node_(std::move(to_node)),
      direction_(direction),
      types_(std::move(types)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
  AddSolvedSymbol(from_node_);
  AddSolvedSymbol(relationship_);
  AddSolvedSymbol(to_node_);
  AddOutputColumn(from_node_);
  AddOutputColumn(relationship_);
  AddOutputColumn(to_node_);
}

std::string ExpandIntoPlan::Details() const {
  return RelationshipDetails(from_node_, relationship_, to_node_, direction_,
                             types_);
}

VarExpandPlan::VarExpandPlan(LogicalPlanPtr source, std::string from_node,
                             std::string relationship, std::string to_node,
                             ExpandDirection direction,
                             std::vector<std::string> types,
                             LogicalVariableLength length)
    : LogicalPlan(LogicalPlanNodeType::kVarExpand,
                  UnaryChildren(std::move(source), "VarExpand")),
      from_node_(std::move(from_node)),
      relationship_(std::move(relationship)),
      to_node_(std::move(to_node)),
      direction_(direction),
      types_(std::move(types)),
      length_(std::move(length)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
  AddSolvedSymbol(from_node_);
  AddSolvedSymbol(relationship_);
  AddSolvedSymbol(to_node_);
  AddOutputColumn(from_node_);
  AddOutputColumn(relationship_);
  AddOutputColumn(to_node_);
}

std::string VarExpandPlan::Details() const {
  return VarRelationshipDetails(from_node_, relationship_, to_node_, direction_,
                                types_, length_);
}

PathBuildPlan::PathBuildPlan(LogicalPlanPtr source, std::string path_variable)
    : LogicalPlan(LogicalPlanNodeType::kPathBuild,
                  UnaryChildren(std::move(source), "PathBuild")),
      path_variable_(std::move(path_variable)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
  AddSolvedSymbol(path_variable_);
  AddOutputColumn(path_variable_);
}

std::string PathBuildPlan::Details() const { return path_variable_; }

FilterPlan::FilterPlan(LogicalPlanPtr source, const ast::Expression *predicate)
    : LogicalPlan(LogicalPlanNodeType::kFilter,
                  UnaryChildren(std::move(source), "Filter")),
      predicate_(predicate) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string FilterPlan::Details() const {
  if (predicate_ == nullptr) {
    return "null";
  }
  return ast::ExpressionToString(*predicate_);
}

ProjectionPlan::ProjectionPlan(LogicalPlanPtr source,
                               std::vector<LogicalProjectionItem> items)
    : LogicalPlan(LogicalPlanNodeType::kProjection,
                  UnaryChildren(std::move(source), "Projection")),
      items_(std::move(items)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  for (const auto &item : items_) {
    AddSolvedSymbol(item.alias);
    AddOutputColumn(item.alias);
  }
}

std::string ProjectionPlan::Details() const {
  return Join(ProjectionItemAliases(items_), ", ");
}

DistinctPlan::DistinctPlan(LogicalPlanPtr source,
                           std::vector<LogicalProjectionItem> grouping_items)
    : LogicalPlan(LogicalPlanNodeType::kDistinct,
                  UnaryChildren(std::move(source), "Distinct")),
      grouping_items_(std::move(grouping_items)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  for (const auto &item : grouping_items_) {
    AddSolvedSymbol(item.alias);
    AddOutputColumn(item.alias);
  }
}

std::string DistinctPlan::Details() const {
  return Join(ProjectionItemAliases(grouping_items_), ", ");
}

AggregationPlan::AggregationPlan(
    LogicalPlanPtr source, std::vector<LogicalProjectionItem> grouping_items,
    std::vector<LogicalProjectionItem> aggregation_items)
    : LogicalPlan(LogicalPlanNodeType::kAggregation,
                  UnaryChildren(std::move(source), "Aggregation")),
      grouping_items_(std::move(grouping_items)),
      aggregation_items_(std::move(aggregation_items)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  for (const auto &item : grouping_items_) {
    AddSolvedSymbol(item.alias);
    AddOutputColumn(item.alias);
  }
  for (const auto &item : aggregation_items_) {
    AddSolvedSymbol(item.alias);
    AddOutputColumn(item.alias);
  }
}

std::string AggregationPlan::Details() const {
  return Join(ProjectionItemAliases(grouping_items_, aggregation_items_), ", ");
}

SortPlan::SortPlan(LogicalPlanPtr source, std::vector<LogicalSortItem> items)
    : LogicalPlan(LogicalPlanNodeType::kSort,
                  UnaryChildren(std::move(source), "Sort")),
      items_(std::move(items)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string SortPlan::Details() const {
  std::vector<std::string> items;
  items.reserve(items_.size());
  for (const auto &item : items_) {
    std::string expression = item.expression != nullptr
                                 ? ast::ExpressionToString(*item.expression)
                                 : "null";
    expression.push_back(' ');
    expression.append(ToString(item.direction));
    items.push_back(std::move(expression));
  }
  return Join(items, ", ");
}

SkipPlan::SkipPlan(LogicalPlanPtr source, const ast::Expression *skip)
    : LogicalPlan(LogicalPlanNodeType::kSkip,
                  UnaryChildren(std::move(source), "Skip")),
      skip_(skip) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string SkipPlan::Details() const {
  if (skip_ == nullptr) {
    return "null";
  }
  return ast::ExpressionToString(*skip_);
}

LimitPlan::LimitPlan(LogicalPlanPtr source, const ast::Expression *limit)
    : LogicalPlan(LogicalPlanNodeType::kLimit,
                  UnaryChildren(std::move(source), "Limit")),
      limit_(limit) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string LimitPlan::Details() const {
  if (limit_ == nullptr) {
    return "null";
  }
  return ast::ExpressionToString(*limit_);
}

ProduceResultsPlan::ProduceResultsPlan(LogicalPlanPtr source,
                                       std::vector<std::string> columns)
    : LogicalPlan(LogicalPlanNodeType::kProduceResults,
                  UnaryChildren(std::move(source), "ProduceResults")) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(std::move(columns));
}

std::string ProduceResultsPlan::Details() const {
  return Join(OutputColumns(), ", ");
}

CartesianProductPlan::CartesianProductPlan(LogicalPlanPtr left,
                                           LogicalPlanPtr right)
    : LogicalPlan(LogicalPlanNodeType::kCartesianProduct,
                  BinaryChildren(std::move(left), std::move(right),
                                 "CartesianProduct")) {
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  SetOutputColumns(UnionOutputColumns(Child(0), Child(1)));
}

NodeHashJoinPlan::NodeHashJoinPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                                   std::vector<std::string> join_keys)
    : LogicalPlan(
          LogicalPlanNodeType::kNodeHashJoin,
          BinaryChildren(std::move(left), std::move(right), "NodeHashJoin")),
      join_keys_(std::move(join_keys)) {
  CHECK(!join_keys_.empty(), common::InvalidArgumentError,
        "node hash join keys are empty");
  for (const auto &key : join_keys_) {
    CHECK(Child(0).SolvedSymbols().contains(key) &&
              Child(1).SolvedSymbols().contains(key),
          common::InvalidArgumentError,
          "node hash join key is not solved by both inputs");
  }
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  SetOutputColumns(UnionOutputColumns(Child(0), Child(1)));
}

std::string NodeHashJoinPlan::Details() const { return Join(join_keys_, ", "); }

ApplyPlan::ApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right)
    : LogicalPlan(LogicalPlanNodeType::kApply,
                  BinaryChildren(std::move(left), std::move(right), "Apply")) {
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  SetOutputColumns(UnionOutputColumns(Child(0), Child(1)));
}

SemiApplyPlan::SemiApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right)
    : LogicalPlan(
          LogicalPlanNodeType::kSemiApply,
          BinaryChildren(std::move(left), std::move(right), "SemiApply")) {
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  SetOutputColumns(Child(0).OutputColumns());
}

LetSemiApplyPlan::LetSemiApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                                   std::string value_variable)
    : LogicalPlan(
          LogicalPlanNodeType::kLetSemiApply,
          BinaryChildren(std::move(left), std::move(right), "LetSemiApply")),
      value_variable_(std::move(value_variable)) {
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  AddSolvedSymbol(value_variable_);
  SetOutputColumns(Child(0).OutputColumns());
  AddOutputColumn(value_variable_);
}

std::string LetSemiApplyPlan::Details() const { return value_variable_; }

RollUpApplyPlan::RollUpApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                                 std::string collection_variable,
                                 std::string value_variable)
    : LogicalPlan(
          LogicalPlanNodeType::kRollUpApply,
          BinaryChildren(std::move(left), std::move(right), "RollUpApply")),
      collection_variable_(std::move(collection_variable)),
      value_variable_(std::move(value_variable)) {
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  AddSolvedSymbol(collection_variable_);
  AddSolvedSymbol(value_variable_);
  SetOutputColumns(Child(0).OutputColumns());
  AddOutputColumn(collection_variable_);
}

std::string RollUpApplyPlan::Details() const {
  if (value_variable_.empty()) {
    return collection_variable_;
  }
  return collection_variable_ + " <- " + value_variable_;
}

OptionalApplyPlan::OptionalApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right)
    : LogicalPlan(
          LogicalPlanNodeType::kOptionalApply,
          BinaryChildren(std::move(left), std::move(right), "OptionalApply")) {
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  SetOutputColumns(UnionOutputColumns(Child(0), Child(1)));
}

AssertIsNodePlan::AssertIsNodePlan(LogicalPlanPtr source,
                                   std::vector<std::string> variables)
    : LogicalPlan(LogicalPlanNodeType::kAssertIsNode,
                  UnaryChildren(std::move(source), "AssertIsNode")),
      variables_(std::move(variables)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string AssertIsNodePlan::Details() const { return Join(variables_, ", "); }

UnwindPlan::UnwindPlan(LogicalPlanPtr source, const ast::Expression *expression,
                       std::string alias)
    : LogicalPlan(LogicalPlanNodeType::kUnwind,
                  UnaryChildren(std::move(source), "Unwind")),
      expression_(expression),
      alias_(std::move(alias)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
  AddSolvedSymbol(alias_);
  AddOutputColumn(alias_);
}

std::string UnwindPlan::Details() const {
  std::string expression =
      expression_ != nullptr ? ast::ExpressionToString(*expression_) : "null";
  expression.append(" AS ");
  expression.append(alias_);
  return expression;
}

UnionPlan::UnionPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                     std::vector<LogicalUnionMapping> mappings, bool all)
    : LogicalPlan(LogicalPlanNodeType::kUnion,
                  BinaryChildren(std::move(left), std::move(right), "Union")),
      mappings_(std::move(mappings)),
      all_(all) {
  SetOutputColumns(UnionOutputVariables(mappings_));
  SetSolvedSymbols(SymbolsFromColumns(OutputColumns()));
}

std::string UnionPlan::Details() const {
  std::string details = all_ ? "ALL" : "DISTINCT";
  const std::vector<std::string> columns = UnionOutputVariables(mappings_);
  if (!columns.empty()) {
    details.append(" ");
    details.append(Join(columns, ", "));
  }
  return details;
}

}  // namespace ir
