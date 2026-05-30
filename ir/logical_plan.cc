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
    std::string_view{"NodeIndexSeek"},
    std::string_view{"NodeIndexRangeSeek"},
    std::string_view{"RelationshipTypeScan"},
    std::string_view{"RelationshipIndexSeek"},
    std::string_view{"RelationshipIndexRangeSeek"},
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
    std::string_view{"ValueHashJoin"},
    std::string_view{"PredicateJoin"},
    std::string_view{"Apply"},
    std::string_view{"SemiApply"},
    std::string_view{"AntiSemiApply"},
    std::string_view{"LetSemiApply"},
    std::string_view{"RollUpApply"},
    std::string_view{"OptionalApply"},
    std::string_view{"AssertIsNode"},
    std::string_view{"WriteBarrier"},
    std::string_view{"CreateNode"},
    std::string_view{"CreateRelationship"},
    std::string_view{"Merge"},
    std::string_view{"SetProperty"},
    std::string_view{"SetProperties"},
    std::string_view{"SetLabels"},
    std::string_view{"RemoveProperty"},
    std::string_view{"RemoveLabels"},
    std::string_view{"Delete"},
    std::string_view{"DetachDelete"},
    std::string_view{"Unwind"},
    std::string_view{"ProcedureCall"},
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

std::string ProcedureYieldItemDetails(const ProcedureYieldItem &item) {
  if (item.result_field.has_value() && *item.result_field != item.variable) {
    return *item.result_field + " AS " + item.variable;
  }
  return item.variable;
}

std::vector<std::string> ProcedureYieldItemDetails(
    const std::vector<ProcedureYieldItem> &items) {
  std::vector<std::string> details;
  details.reserve(items.size());
  for (const auto &item : items) {
    details.push_back(ProcedureYieldItemDetails(item));
  }
  return details;
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

std::string DirectionalRelationshipDetails(
    std::string_view from_node, std::string_view relationship,
    std::string_view to_node, Direction direction,
    const std::vector<std::string> &types) {
  switch (direction) {
    case Direction::kIncoming:
      return RelationshipDetails(from_node, relationship, to_node,
                                 ExpandDirection::kIncoming, types);
    case Direction::kOutgoing:
      return RelationshipDetails(from_node, relationship, to_node,
                                 ExpandDirection::kOutgoing, types);
    case Direction::kBoth:
      return RelationshipDetails(from_node, relationship, to_node,
                                 ExpandDirection::kBoth, types);
  }
  THROW(common::InternalError, "unknown relationship direction");
}

std::string NodePatternDetails(std::string_view variable,
                               const std::vector<std::string> &labels) {
  std::string out(variable);
  for (const auto &label : labels) {
    out.push_back(':');
    out.append(label);
  }
  return out;
}

std::string ExpressionDetail(const ast::Expression *expression) {
  return expression != nullptr ? ast::ExpressionToString(*expression) : "null";
}

std::vector<std::string> ExpressionDetails(
    const std::vector<const ast::Expression *> &expressions) {
  std::vector<std::string> details;
  details.reserve(expressions.size());
  for (const ast::Expression *expression : expressions) {
    details.push_back(ExpressionDetail(expression));
  }
  return details;
}

std::string PropertyValueDetails(std::string_view variable,
                                 std::string_view property_key,
                                 const ast::Expression *value_expression) {
  std::string out(variable);
  out.push_back('.');
  out.append(property_key);
  out.append(" = ");
  out.append(value_expression != nullptr
                 ? ast::ExpressionToString(*value_expression)
                 : "null");
  return out;
}

std::string PropertyMapDetails(const PatternPropertyMap &properties) {
  std::vector<std::string> entries;
  entries.reserve(properties.entries.size() +
                  (properties.parameter != nullptr ? 1 : 0));
  for (const auto &entry : properties.entries) {
    entries.push_back(entry.key + ": " + ExpressionDetail(entry.value));
  }
  if (properties.parameter != nullptr) {
    entries.push_back(ExpressionDetail(properties.parameter));
  }
  if (entries.empty()) {
    return {};
  }
  if (properties.parameter != nullptr && properties.entries.empty()) {
    return entries.front();
  }
  return "{" + Join(entries, ", ") + "}";
}

std::string CreateNodeDetails(const CreateNodePattern &node) {
  std::string out = NodePatternDetails(node.variable, node.labels);
  const std::string properties = PropertyMapDetails(node.properties);
  if (!properties.empty()) {
    out.push_back(' ');
    out.append(properties);
  }
  return out;
}

std::string RelationshipDetailsWithProperties(
    std::string_view from_node, std::string_view relationship,
    std::string_view to_node, Direction direction,
    const std::vector<std::string> &types,
    const PatternPropertyMap &properties) {
  std::string out = DirectionalRelationshipDetails(from_node, relationship,
                                                   to_node, direction, types);
  const std::string property_details = PropertyMapDetails(properties);
  if (property_details.empty()) {
    return out;
  }
  (void)relationship;
  const std::size_t position = out.find(']');
  CHECK(position != std::string::npos, common::InternalError,
        "relationship detail is missing relationship close");
  out.insert(position, " " + property_details);
  return out;
}

std::string CreateRelationshipDetails(
    const CreateRelationshipPattern &relationship) {
  return RelationshipDetailsWithProperties(
      relationship.left_node, relationship.variable, relationship.right_node,
      relationship.direction, relationship.types, relationship.properties);
}

const CreateNodePattern *FindCreateNode(const CreatePattern &pattern,
                                        std::string_view variable) {
  for (const auto &node : pattern.nodes) {
    if (node.variable == variable) {
      return &node;
    }
  }
  return nullptr;
}

std::string MergeEndpointDetails(const CreatePattern &pattern,
                                 std::string_view variable) {
  const CreateNodePattern *node = FindCreateNode(pattern, variable);
  if (node == nullptr) {
    return std::string(variable);
  }
  return CreateNodeDetails(*node);
}

std::string MergeRelationshipDetails(
    const CreatePattern &pattern,
    const CreateRelationshipPattern &relationship) {
  return RelationshipDetailsWithProperties(
      MergeEndpointDetails(pattern, relationship.left_node),
      relationship.variable,
      MergeEndpointDetails(pattern, relationship.right_node),
      relationship.direction, relationship.types, relationship.properties);
}

std::string CreatePatternDetails(const CreatePattern &pattern) {
  std::vector<std::string> parts;
  std::unordered_set<std::string> relationship_nodes;
  for (const auto &relationship : pattern.relationships) {
    parts.push_back(MergeRelationshipDetails(pattern, relationship));
    AddSymbol(&relationship_nodes, relationship.left_node);
    AddSymbol(&relationship_nodes, relationship.right_node);
  }
  for (const auto &node : pattern.nodes) {
    if (!relationship_nodes.contains(node.variable)) {
      parts.push_back(CreateNodeDetails(node));
    }
  }
  for (const auto &path_variable : pattern.path_variables) {
    parts.push_back(path_variable);
  }
  return Join(parts, ", ");
}

std::unordered_set<std::string> CreatePatternSolvedSymbols(
    const CreatePattern &pattern) {
  std::unordered_set<std::string> symbols;
  for (const auto &path_variable : pattern.path_variables) {
    AddSymbol(&symbols, path_variable);
  }
  for (const auto &node : pattern.nodes) {
    AddSymbol(&symbols, node.variable);
  }
  for (const auto &relationship : pattern.relationships) {
    AddSymbol(&symbols, relationship.left_node);
    AddSymbol(&symbols, relationship.variable);
    AddSymbol(&symbols, relationship.right_node);
  }
  return symbols;
}

std::string SetPatternDetails(const SetMutatingPattern &pattern) {
  switch (pattern.kind) {
    case SetMutatingPatternKind::kSetProperty:
      return ExpressionDetail(pattern.entity) + "." + pattern.property_key +
             " = " + ExpressionDetail(pattern.value);
    case SetMutatingPatternKind::kSetExactPropertiesFromMap:
      return ExpressionDetail(pattern.entity) + " = " +
             ExpressionDetail(pattern.value);
    case SetMutatingPatternKind::kSetIncludingPropertiesFromMap:
      return ExpressionDetail(pattern.entity) +
             " += " + ExpressionDetail(pattern.value);
    case SetMutatingPatternKind::kSetLabels:
      return ExpressionDetail(pattern.entity) + ":" + Join(pattern.labels, ":");
  }
  THROW(common::InternalError, "unknown SET pattern kind");
}

std::string RemovePatternDetails(const RemoveMutatingPattern &pattern) {
  switch (pattern.kind) {
    case RemoveMutatingPatternKind::kRemoveProperty:
      return ExpressionDetail(pattern.entity) + "." + pattern.property_key;
    case RemoveMutatingPatternKind::kRemoveLabels:
      return ExpressionDetail(pattern.entity) + ":" + Join(pattern.labels, ":");
  }
  THROW(common::InternalError, "unknown REMOVE pattern kind");
}

std::string MergeActionDetails(const MergeActionPattern &action) {
  std::vector<std::string> set_details;
  set_details.reserve(action.set_patterns.size());
  for (const auto &pattern : action.set_patterns) {
    set_details.push_back(SetPatternDetails(pattern));
  }
  std::string out = action.on_match ? "ON MATCH SET " : "ON CREATE SET ";
  out.append(Join(set_details, ", "));
  return out;
}

std::string MergeDetails(const MergePattern &merge) {
  std::vector<std::string> details;
  details.push_back(CreatePatternDetails(merge.create_pattern));
  for (const auto &action : merge.actions) {
    details.push_back(MergeActionDetails(action));
  }
  return Join(details, " ");
}

std::string NodeIndexDetails(std::string_view variable,
                             const std::vector<std::string> &labels,
                             std::string_view property_key,
                             const ast::Expression *value_expression) {
  std::string out = NodePatternDetails(variable, labels);
  out.append(" WHERE ");
  out.append(PropertyValueDetails(variable, property_key, value_expression));
  return out;
}

std::string NodeIndexRangeDetails(
    std::string_view variable, const std::vector<std::string> &labels,
    const std::vector<const ast::Expression *> &predicates) {
  std::string out = NodePatternDetails(variable, labels);
  out.append(" WHERE ");
  out.append(Join(ExpressionDetails(predicates), " AND "));
  return out;
}

std::string RelationshipIndexDetails(std::string_view from_node,
                                     std::string_view relationship,
                                     std::string_view to_node,
                                     ExpandDirection direction,
                                     const std::vector<std::string> &types,
                                     std::string_view property_key,
                                     const ast::Expression *value_expression) {
  std::string out =
      RelationshipDetails(from_node, relationship, to_node, direction, types);
  out.append(" WHERE ");
  out.append(
      PropertyValueDetails(relationship, property_key, value_expression));
  return out;
}

std::string RelationshipIndexRangeDetails(
    std::string_view from_node, std::string_view relationship,
    std::string_view to_node, ExpandDirection direction,
    const std::vector<std::string> &types,
    const std::vector<const ast::Expression *> &predicates) {
  std::string out =
      RelationshipDetails(from_node, relationship, to_node, direction, types);
  out.append(" WHERE ");
  out.append(Join(ExpressionDetails(predicates), " AND "));
  return out;
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

void LogicalPlan::SetCostEstimate(double estimated_rows, double cost) {
  metadata_.estimated_rows = estimated_rows;
  metadata_.cost = cost;
}

void LogicalPlan::ClearCostEstimate() {
  metadata_.estimated_rows.reset();
  metadata_.cost.reset();
}

void LogicalPlan::SetOrderingTrait(std::vector<LogicalSortItem> ordering) {
  metadata_.ordering = std::move(ordering);
}

void LogicalPlan::ClearOrderingTrait() { metadata_.ordering.clear(); }

void LogicalPlan::SetDistinctTrait(bool distinct) {
  metadata_.distinct = distinct;
}

void LogicalPlan::CopyMetadataFrom(const LogicalPlan &source) {
  metadata_ = source.metadata_;
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
    : NodeByLabelScanPlan(std::move(variable),
                          std::vector<std::string>{std::move(label)}) {}

NodeByLabelScanPlan::NodeByLabelScanPlan(std::string variable,
                                         std::vector<std::string> labels)
    : LogicalPlan(LogicalPlanNodeType::kNodeByLabelScan),
      variable_(std::move(variable)),
      labels_(std::move(labels)) {
  CHECK(!labels_.empty(), common::InvalidArgumentError,
        "node label scan labels are empty");
  AddOutputColumn(variable_);
  AddSolvedSymbol(variable_);
}

const std::string &NodeByLabelScanPlan::Label() const noexcept {
  return labels_.front();
}

std::string NodeByLabelScanPlan::Details() const {
  return NodePatternDetails(variable_, labels_);
}

NodeIndexSeekPlan::NodeIndexSeekPlan(std::string variable,
                                     std::vector<std::string> labels,
                                     std::string property_key,
                                     const ast::Expression *value_expression)
    : LogicalPlan(LogicalPlanNodeType::kNodeIndexSeek),
      variable_(std::move(variable)),
      labels_(std::move(labels)),
      property_key_(std::move(property_key)),
      value_expression_(value_expression) {
  CHECK(!property_key_.empty(), common::InvalidArgumentError,
        "node index seek property key is empty");
  AddOutputColumn(variable_);
  AddSolvedSymbol(variable_);
}

std::string NodeIndexSeekPlan::Details() const {
  return NodeIndexDetails(variable_, labels_, property_key_, value_expression_);
}

NodeIndexRangeSeekPlan::NodeIndexRangeSeekPlan(
    std::string variable, std::vector<std::string> labels,
    std::string property_key, std::vector<const ast::Expression *> predicates)
    : LogicalPlan(LogicalPlanNodeType::kNodeIndexRangeSeek),
      variable_(std::move(variable)),
      labels_(std::move(labels)),
      property_key_(std::move(property_key)),
      predicates_(std::move(predicates)) {
  CHECK(!property_key_.empty(), common::InvalidArgumentError,
        "node index range seek property key is empty");
  CHECK(!predicates_.empty(), common::InvalidArgumentError,
        "node index range seek predicates are empty");
  AddOutputColumn(variable_);
  AddSolvedSymbol(variable_);
}

std::string NodeIndexRangeSeekPlan::Details() const {
  return NodeIndexRangeDetails(variable_, labels_, predicates_);
}

RelationshipTypeScanPlan::RelationshipTypeScanPlan(
    std::string from_node, std::string relationship, std::string to_node,
    ExpandDirection direction, std::vector<std::string> types)
    : LogicalPlan(LogicalPlanNodeType::kRelationshipTypeScan),
      from_node_(std::move(from_node)),
      relationship_(std::move(relationship)),
      to_node_(std::move(to_node)),
      direction_(direction),
      types_(std::move(types)) {
  CHECK(!types_.empty(), common::InvalidArgumentError,
        "relationship type scan types are empty");
  AddOutputColumn(from_node_);
  AddOutputColumn(relationship_);
  AddOutputColumn(to_node_);
  AddSolvedSymbol(from_node_);
  AddSolvedSymbol(relationship_);
  AddSolvedSymbol(to_node_);
}

std::string RelationshipTypeScanPlan::Details() const {
  return RelationshipDetails(from_node_, relationship_, to_node_, direction_,
                             types_);
}

RelationshipIndexSeekPlan::RelationshipIndexSeekPlan(
    std::string from_node, std::string relationship, std::string to_node,
    ExpandDirection direction, std::vector<std::string> types,
    std::string property_key, const ast::Expression *value_expression)
    : LogicalPlan(LogicalPlanNodeType::kRelationshipIndexSeek),
      from_node_(std::move(from_node)),
      relationship_(std::move(relationship)),
      to_node_(std::move(to_node)),
      direction_(direction),
      types_(std::move(types)),
      property_key_(std::move(property_key)),
      value_expression_(value_expression) {
  CHECK(!property_key_.empty(), common::InvalidArgumentError,
        "relationship index seek property key is empty");
  AddOutputColumn(from_node_);
  AddOutputColumn(relationship_);
  AddOutputColumn(to_node_);
  AddSolvedSymbol(from_node_);
  AddSolvedSymbol(relationship_);
  AddSolvedSymbol(to_node_);
}

std::string RelationshipIndexSeekPlan::Details() const {
  return RelationshipIndexDetails(from_node_, relationship_, to_node_,
                                  direction_, types_, property_key_,
                                  value_expression_);
}

RelationshipIndexRangeSeekPlan::RelationshipIndexRangeSeekPlan(
    std::string from_node, std::string relationship, std::string to_node,
    ExpandDirection direction, std::vector<std::string> types,
    std::string property_key, std::vector<const ast::Expression *> predicates)
    : LogicalPlan(LogicalPlanNodeType::kRelationshipIndexRangeSeek),
      from_node_(std::move(from_node)),
      relationship_(std::move(relationship)),
      to_node_(std::move(to_node)),
      direction_(direction),
      types_(std::move(types)),
      property_key_(std::move(property_key)),
      predicates_(std::move(predicates)) {
  CHECK(!property_key_.empty(), common::InvalidArgumentError,
        "relationship index range seek property key is empty");
  CHECK(!predicates_.empty(), common::InvalidArgumentError,
        "relationship index range seek predicates are empty");
  AddOutputColumn(from_node_);
  AddOutputColumn(relationship_);
  AddOutputColumn(to_node_);
  AddSolvedSymbol(from_node_);
  AddSolvedSymbol(relationship_);
  AddSolvedSymbol(to_node_);
}

std::string RelationshipIndexRangeSeekPlan::Details() const {
  return RelationshipIndexRangeDetails(from_node_, relationship_, to_node_,
                                       direction_, types_, predicates_);
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
  SetOutputColumns(std::move(columns));
  SetSolvedSymbols(SymbolsFromColumns(OutputColumns()));
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

ValueHashJoinPlan::ValueHashJoinPlan(
    LogicalPlanPtr left, LogicalPlanPtr right,
    std::vector<const ast::Expression *> predicates)
    : LogicalPlan(
          LogicalPlanNodeType::kValueHashJoin,
          BinaryChildren(std::move(left), std::move(right), "ValueHashJoin")),
      predicates_(std::move(predicates)) {
  CHECK(!predicates_.empty(), common::InvalidArgumentError,
        "value hash join predicates are empty");
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  SetOutputColumns(UnionOutputColumns(Child(0), Child(1)));
}

std::string ValueHashJoinPlan::Details() const {
  return Join(ExpressionDetails(predicates_), " AND ");
}

PredicateJoinPlan::PredicateJoinPlan(
    LogicalPlanPtr left, LogicalPlanPtr right,
    std::vector<const ast::Expression *> predicates)
    : LogicalPlan(
          LogicalPlanNodeType::kPredicateJoin,
          BinaryChildren(std::move(left), std::move(right), "PredicateJoin")),
      predicates_(std::move(predicates)) {
  CHECK(!predicates_.empty(), common::InvalidArgumentError,
        "predicate join predicates are empty");
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  SetOutputColumns(UnionOutputColumns(Child(0), Child(1)));
}

std::string PredicateJoinPlan::Details() const {
  return Join(ExpressionDetails(predicates_), " AND ");
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
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

AntiSemiApplyPlan::AntiSemiApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right)
    : LogicalPlan(
          LogicalPlanNodeType::kAntiSemiApply,
          BinaryChildren(std::move(left), std::move(right), "AntiSemiApply")) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

LetSemiApplyPlan::LetSemiApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                                   std::string value_variable)
    : LogicalPlan(
          LogicalPlanNodeType::kLetSemiApply,
          BinaryChildren(std::move(left), std::move(right), "LetSemiApply")),
      value_variable_(std::move(value_variable)) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
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
  SetSolvedSymbols(Child(0).SolvedSymbols());
  AddSolvedSymbol(collection_variable_);
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

WriteBarrierPlan::WriteBarrierPlan(LogicalPlanPtr source)
    : LogicalPlan(LogicalPlanNodeType::kWriteBarrier,
                  UnaryChildren(std::move(source), "WriteBarrier")) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

CreateNodePlan::CreateNodePlan(LogicalPlanPtr source, CreateNodePattern node)
    : LogicalPlan(LogicalPlanNodeType::kCreateNode,
                  UnaryChildren(std::move(source), "CreateNode")),
      node_(std::move(node)) {
  CHECK(!node_.variable.empty(), common::InvalidArgumentError,
        "created node variable is empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
  AddSolvedSymbol(node_.variable);
  AddOutputColumn(node_.variable);
}

std::string CreateNodePlan::Details() const { return CreateNodeDetails(node_); }

CreateRelationshipPlan::CreateRelationshipPlan(
    LogicalPlanPtr source, CreateRelationshipPattern relationship)
    : LogicalPlan(LogicalPlanNodeType::kCreateRelationship,
                  UnaryChildren(std::move(source), "CreateRelationship")),
      relationship_(std::move(relationship)) {
  CHECK(!relationship_.variable.empty(), common::InvalidArgumentError,
        "created relationship variable is empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
  AddSolvedSymbol(relationship_.left_node);
  AddSolvedSymbol(relationship_.variable);
  AddSolvedSymbol(relationship_.right_node);
  AddOutputColumn(relationship_.left_node);
  AddOutputColumn(relationship_.variable);
  AddOutputColumn(relationship_.right_node);
}

std::string CreateRelationshipPlan::Details() const {
  return CreateRelationshipDetails(relationship_);
}

MergePlan::MergePlan(LogicalPlanPtr source, LogicalPlanPtr match_plan,
                     MergePattern merge)
    : LogicalPlan(
          LogicalPlanNodeType::kMerge,
          BinaryChildren(std::move(source), std::move(match_plan), "Merge")),
      merge_(std::move(merge)) {
  SetSolvedSymbols(UnionSolvedSymbols(Child(0), Child(1)));
  SetOutputColumns(UnionOutputColumns(Child(0), Child(1)));
  std::unordered_set<std::string> symbols =
      CreatePatternSolvedSymbols(merge_.create_pattern);
  for (const auto &symbol : symbols) {
    AddSolvedSymbol(symbol);
    AddOutputColumn(symbol);
  }
}

std::string MergePlan::Details() const { return MergeDetails(merge_); }

SetPropertyPlan::SetPropertyPlan(LogicalPlanPtr source,
                                 const ast::Expression *entity,
                                 std::string property_key,
                                 const ast::Expression *value)
    : LogicalPlan(LogicalPlanNodeType::kSetProperty,
                  UnaryChildren(std::move(source), "SetProperty")),
      entity_(entity),
      property_key_(std::move(property_key)),
      value_(value) {
  CHECK(!property_key_.empty(), common::InvalidArgumentError,
        "SET property key is empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string SetPropertyPlan::Details() const {
  return ExpressionDetail(entity_) + "." + property_key_ + " = " +
         ExpressionDetail(value_);
}

SetPropertiesPlan::SetPropertiesPlan(LogicalPlanPtr source,
                                     const ast::Expression *entity,
                                     const ast::Expression *value,
                                     bool include_existing)
    : LogicalPlan(LogicalPlanNodeType::kSetProperties,
                  UnaryChildren(std::move(source), "SetProperties")),
      entity_(entity),
      value_(value),
      include_existing_(include_existing) {
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string SetPropertiesPlan::Details() const {
  return ExpressionDetail(entity_) + (include_existing_ ? " += " : " = ") +
         ExpressionDetail(value_);
}

SetLabelsPlan::SetLabelsPlan(LogicalPlanPtr source,
                             const ast::Expression *entity,
                             std::vector<std::string> labels)
    : LogicalPlan(LogicalPlanNodeType::kSetLabels,
                  UnaryChildren(std::move(source), "SetLabels")),
      entity_(entity),
      labels_(std::move(labels)) {
  CHECK(!labels_.empty(), common::InvalidArgumentError,
        "SET labels list is empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string SetLabelsPlan::Details() const {
  return ExpressionDetail(entity_) + ":" + Join(labels_, ":");
}

RemovePropertyPlan::RemovePropertyPlan(LogicalPlanPtr source,
                                       const ast::Expression *entity,
                                       std::string property_key)
    : LogicalPlan(LogicalPlanNodeType::kRemoveProperty,
                  UnaryChildren(std::move(source), "RemoveProperty")),
      entity_(entity),
      property_key_(std::move(property_key)) {
  CHECK(!property_key_.empty(), common::InvalidArgumentError,
        "REMOVE property key is empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string RemovePropertyPlan::Details() const {
  return ExpressionDetail(entity_) + "." + property_key_;
}

RemoveLabelsPlan::RemoveLabelsPlan(LogicalPlanPtr source,
                                   const ast::Expression *entity,
                                   std::vector<std::string> labels)
    : LogicalPlan(LogicalPlanNodeType::kRemoveLabels,
                  UnaryChildren(std::move(source), "RemoveLabels")),
      entity_(entity),
      labels_(std::move(labels)) {
  CHECK(!labels_.empty(), common::InvalidArgumentError,
        "REMOVE labels list is empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string RemoveLabelsPlan::Details() const {
  return ExpressionDetail(entity_) + ":" + Join(labels_, ":");
}

DeletePlan::DeletePlan(LogicalPlanPtr source,
                       std::vector<const ast::Expression *> expressions)
    : LogicalPlan(LogicalPlanNodeType::kDelete,
                  UnaryChildren(std::move(source), "Delete")),
      expressions_(std::move(expressions)) {
  CHECK(!expressions_.empty(), common::InvalidArgumentError,
        "DELETE expressions are empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string DeletePlan::Details() const {
  return Join(ExpressionDetails(expressions_), ", ");
}

DetachDeletePlan::DetachDeletePlan(
    LogicalPlanPtr source, std::vector<const ast::Expression *> expressions)
    : LogicalPlan(LogicalPlanNodeType::kDetachDelete,
                  UnaryChildren(std::move(source), "DetachDelete")),
      expressions_(std::move(expressions)) {
  CHECK(!expressions_.empty(), common::InvalidArgumentError,
        "DETACH DELETE expressions are empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
}

std::string DetachDeletePlan::Details() const {
  return Join(ExpressionDetails(expressions_), ", ");
}

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

ProcedureCallPlan::ProcedureCallPlan(
    LogicalPlanPtr source, std::string procedure_name,
    std::vector<const ast::Expression *> arguments,
    std::vector<ProcedureYieldItem> yield_items, bool yield_star,
    bool read_only)
    : LogicalPlan(LogicalPlanNodeType::kProcedureCall,
                  UnaryChildren(std::move(source), "ProcedureCall")),
      procedure_name_(std::move(procedure_name)),
      arguments_(std::move(arguments)),
      yield_items_(std::move(yield_items)),
      yield_star_(yield_star),
      read_only_(read_only) {
  CHECK(!procedure_name_.empty(), common::InvalidArgumentError,
        "procedure name is empty");
  SetSolvedSymbols(Child(0).SolvedSymbols());
  SetOutputColumns(Child(0).OutputColumns());
  for (const auto &item : yield_items_) {
    CHECK(!item.variable.empty(), common::InvalidArgumentError,
          "procedure yield variable is empty");
    AddSolvedSymbol(item.variable);
    AddOutputColumn(item.variable);
  }
}

std::string ProcedureCallPlan::Details() const {
  std::vector<std::string> args;
  args.reserve(arguments_.size());
  for (const ast::Expression *argument : arguments_) {
    args.push_back(ExpressionDetail(argument));
  }

  std::string details =
      "CALL " + procedure_name_ + "(" + Join(args, ", ") + ")";
  if (yield_star_) {
    details.append(" YIELD *");
  } else if (!yield_items_.empty()) {
    details.append(" YIELD ");
    details.append(Join(ProcedureYieldItemDetails(yield_items_), ", "));
  }
  return details;
}

}  // namespace ir
