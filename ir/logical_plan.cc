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
    std::string_view{"Filter"},
    std::string_view{"Projection"},
    std::string_view{"Distinct"},
    std::string_view{"Sort"},
    std::string_view{"Skip"},
    std::string_view{"Limit"},
    std::string_view{"ProduceResults"},
    std::string_view{"CartesianProduct"},
    std::string_view{"Apply"},
    std::string_view{"SemiApply"},
};

static_assert(kLogicalPlanNodeTypeNames.size() ==
              static_cast<std::size_t>(LogicalPlanNodeType::kSemiApply) + 1);

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
  std::ostringstream out;
  out << "(" << from_node_ << ")" << ExpandArrow(direction_, true) << "["
      << relationship_;
  if (!types_.empty()) {
    out << ":" << Join(types_, "|");
  }
  out << "]" << ExpandArrow(direction_, false) << "(" << to_node_ << ")";
  return out.str();
}

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

}  // namespace ir
