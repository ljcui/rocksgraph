#include "ir/planner_query.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/expression_dependency.h"
#include "ast/expression_to_string.h"
#include "ast/semantic_table.h"
#include "common/exception.h"

namespace ir {

namespace {

std::string Unsupported(std::string_view feature) {
  return std::string(feature) + " is not supported";
}

std::string Missing(std::string_view subject) {
  return "missing " + std::string(subject);
}

const ast::Expression *UnwrapParenthesized(const ast::Expression *expression) {
  const ast::Expression *unwrapped = expression;
  while (unwrapped != nullptr &&
         unwrapped->Is(ast::ASTNodeType::kParenthesizedExpression)) {
    const auto *parenthesized =
        ast::CastAst<ast::ParenthesizedExpression>(unwrapped);
    CHECK(parenthesized->expr != nullptr, common::InvalidArgumentError,
          "parenthesized expression is null");
    unwrapped = parenthesized->expr.get();
  }
  return unwrapped;
}

void SplitConjunctivePredicates(const ast::Expression *expression,
                                std::vector<const ast::Expression *> *output) {
  CHECK(output != nullptr, common::InternalError, "predicate output is null");
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  CHECK(unwrapped != nullptr, common::InvalidArgumentError,
        "null WHERE predicate is not supported");
  if (unwrapped->Is(ast::ASTNodeType::kAndExpression)) {
    const auto *and_expression = ast::CastAst<ast::AndExpression>(unwrapped);
    SplitConjunctivePredicates(and_expression->left.get(), output);
    SplitConjunctivePredicates(and_expression->right.get(), output);
    return;
  }
  output->push_back(unwrapped);
}

SinglePlannerQuery *LastQueryPart(SinglePlannerQuery *query) {
  SinglePlannerQuery *current = query;
  while (current->tail) {
    current = current->tail.get();
  }
  return current;
}

const SinglePlannerQuery *LastQueryPart(const SinglePlannerQuery *query) {
  const SinglePlannerQuery *current = query;
  while (current->tail) {
    current = current->tail.get();
  }
  return current;
}

const ast::Variable *AsVariableExpression(const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr || !unwrapped->Is(ast::ASTNodeType::kVariable)) {
    return nullptr;
  }
  return ast::CastAst<ast::Variable>(unwrapped);
}

const ast::PropertyExpression *AsPropertyExpression(
    const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr ||
      !unwrapped->Is(ast::ASTNodeType::kPropertyExpression)) {
    return nullptr;
  }
  return ast::CastAst<ast::PropertyExpression>(unwrapped);
}

const ast::ExistentialSubquery *AsExistentialSubquery(
    const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr ||
      !unwrapped->Is(ast::ASTNodeType::kExistentialSubquery)) {
    return nullptr;
  }
  return ast::CastAst<ast::ExistentialSubquery>(unwrapped);
}

void AddNodePatternSymbols(std::unordered_set<std::string> *symbols,
                           const ast::NodePattern *node) {
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
  if (node != nullptr && !node->variable.empty()) {
    symbols->insert(node->variable);
  }
}

void AddRelationshipPatternSymbols(
    std::unordered_set<std::string> *symbols,
    const ast::RelationshipPattern *relationship) {
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
  if (relationship == nullptr || relationship->detail == nullptr ||
      relationship->detail->variable.empty()) {
    return;
  }
  symbols->insert(relationship->detail->variable);
}

void AddPatternElementSymbols(std::unordered_set<std::string> *symbols,
                              const ast::PatternElement *element) {
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
  if (element == nullptr) {
    return;
  }
  AddNodePatternSymbols(symbols, element->node_pattern.get());
  for (const auto &link : element->chain) {
    AddRelationshipPatternSymbols(symbols, link.first.get());
    AddNodePatternSymbols(symbols, link.second.get());
  }
}

void AddPatternPartSymbols(std::unordered_set<std::string> *symbols,
                           const ast::PatternPart *part) {
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
  if (part == nullptr) {
    return;
  }
  if (!part->variable.empty()) {
    symbols->insert(part->variable);
  }
  AddPatternElementSymbols(symbols, part->element.get());
}

void AddPatternSymbols(std::unordered_set<std::string> *symbols,
                       const ast::Pattern *pattern) {
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
  if (pattern == nullptr) {
    return;
  }
  for (const auto &part : pattern->parts) {
    AddPatternPartSymbols(symbols, part.get());
  }
}

std::unordered_set<std::string> MutatingPatternAvailableSymbols(
    const MutatingPattern &mutating_pattern) {
  std::unordered_set<std::string> symbols;
  AddPatternSymbols(&symbols, mutating_pattern.pattern);
  AddPatternPartSymbols(&symbols, mutating_pattern.pattern_part);
  return symbols;
}

std::unordered_set<std::string> QueryGraphLocalAvailableSymbols(
    const QueryGraph &query_graph) {
  std::unordered_set<std::string> symbols = query_graph.argument_ids;
  symbols.insert(query_graph.pattern_paths.begin(),
                 query_graph.pattern_paths.end());
  symbols.insert(query_graph.pattern_nodes.begin(),
                 query_graph.pattern_nodes.end());
  for (const auto &relationship : query_graph.pattern_relationships) {
    CHECK(!relationship.variable.empty(), common::InvalidArgumentError,
          "relationship variable is empty");
    symbols.insert(relationship.variable);
  }
  for (const auto &mutating_pattern : query_graph.mutating_patterns) {
    const std::unordered_set<std::string> mutation_symbols =
        MutatingPatternAvailableSymbols(mutating_pattern);
    symbols.insert(mutation_symbols.begin(), mutation_symbols.end());
  }
  return symbols;
}

std::unordered_set<std::string> QueryGraphAvailableSymbols(
    const QueryGraph &query_graph) {
  std::unordered_set<std::string> symbols =
      QueryGraphLocalAvailableSymbols(query_graph);
  for (const auto &optional_match : query_graph.optional_matches) {
    const std::unordered_set<std::string> optional_symbols =
        QueryGraphAvailableSymbols(optional_match);
    symbols.insert(optional_symbols.begin(), optional_symbols.end());
  }
  return symbols;
}

std::unordered_set<std::string> ProjectionOutputSymbols(
    const ast::SemanticTable &semantic_table,
    const ast::ProjectionBody &projection) {
  std::unordered_set<std::string> symbols;
  const std::vector<std::string> &outputs =
      semantic_table.ProjectionOutputs(projection);
  CHECK(!outputs.empty(), common::InvalidArgumentError,
        "projection output symbols are empty");
  for (const auto &symbol : outputs) {
    CHECK(!symbol.empty(), common::InvalidArgumentError,
          "projection output symbol is empty");
    symbols.insert(symbol);
  }
  return symbols;
}

void AddSymbol(std::unordered_set<std::string> *symbols,
               const std::string &symbol) {
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
  if (!symbol.empty()) {
    symbols->insert(symbol);
  }
}

void AddSymbols(std::unordered_set<std::string> *symbols,
                const std::unordered_set<std::string> &incoming) {
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
  symbols->insert(incoming.begin(), incoming.end());
}

void AddExpressionDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const ast::Expression *expression) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  if (expression == nullptr) {
    return;
  }
  AddSymbols(dependencies, ast::CollectExpressionDependencies(*expression));
}

void AddPropertiesDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const ast::Properties *properties) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  if (properties == nullptr || properties->map == nullptr) {
    return;
  }
  for (const auto &entry : properties->map->entries) {
    AddExpressionDependencySymbols(dependencies, entry.second.get());
  }
}

void AddNodePatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const ast::NodePattern *node) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  if (node == nullptr) {
    return;
  }
  AddSymbol(dependencies, node->variable);
  AddPropertiesDependencySymbols(dependencies, node->properties.get());
}

void AddRelationshipPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const ast::RelationshipPattern *relationship) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  if (relationship == nullptr || relationship->detail == nullptr) {
    return;
  }
  AddSymbol(dependencies, relationship->detail->variable);
  AddPropertiesDependencySymbols(dependencies,
                                 relationship->detail->properties.get());
}

void AddPatternElementDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const ast::PatternElement *element) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  if (element == nullptr) {
    return;
  }
  AddNodePatternDependencySymbols(dependencies, element->node_pattern.get());
  for (const auto &link : element->chain) {
    AddRelationshipPatternDependencySymbols(dependencies, link.first.get());
    AddNodePatternDependencySymbols(dependencies, link.second.get());
  }
}

void AddPatternPartDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const ast::PatternPart *part) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  if (part == nullptr) {
    return;
  }
  AddSymbol(dependencies, part->variable);
  AddPatternElementDependencySymbols(dependencies, part->element.get());
}

void AddPatternDependencySymbols(std::unordered_set<std::string> *dependencies,
                                 const ast::Pattern *pattern) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  if (pattern == nullptr) {
    return;
  }
  for (const auto &part : pattern->parts) {
    AddPatternPartDependencySymbols(dependencies, part.get());
  }
}

void AddSetDependencySymbols(std::unordered_set<std::string> *dependencies,
                             const ast::Set *set) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  if (set == nullptr) {
    return;
  }
  for (const auto &item : set->items) {
    if (item == nullptr) {
      continue;
    }
    AddExpressionDependencySymbols(dependencies, item->target.get());
    AddExpressionDependencySymbols(dependencies, item->value.get());
  }
}

std::unordered_set<std::string> MutatingPatternDependencies(
    const MutatingPattern &mutating_pattern) {
  std::unordered_set<std::string> dependencies;
  switch (mutating_pattern.kind) {
    case MutatingPatternKind::kCreate:
      AddPatternDependencySymbols(&dependencies, mutating_pattern.pattern);
      break;
    case MutatingPatternKind::kMerge:
      AddPatternPartDependencySymbols(&dependencies,
                                      mutating_pattern.pattern_part);
      for (const auto &action : mutating_pattern.merge_actions) {
        AddSetDependencySymbols(&dependencies, action.set);
      }
      break;
    case MutatingPatternKind::kSet:
      for (const auto *item : mutating_pattern.set_items) {
        if (item == nullptr) {
          continue;
        }
        AddExpressionDependencySymbols(&dependencies, item->target.get());
        AddExpressionDependencySymbols(&dependencies, item->value.get());
      }
      break;
    case MutatingPatternKind::kDelete:
      for (const auto *expression : mutating_pattern.delete_expressions) {
        AddExpressionDependencySymbols(&dependencies, expression);
      }
      break;
    case MutatingPatternKind::kRemove:
      for (const auto *item : mutating_pattern.remove_items) {
        if (item == nullptr) {
          continue;
        }
        AddExpressionDependencySymbols(&dependencies, item->target.get());
      }
      break;
  }
  return dependencies;
}

std::unordered_set<std::string> IntersectSymbols(
    const std::unordered_set<std::string> &lhs,
    const std::unordered_set<std::string> &rhs) {
  std::unordered_set<std::string> result;
  const auto &smaller = lhs.size() <= rhs.size() ? lhs : rhs;
  const auto &larger = lhs.size() <= rhs.size() ? rhs : lhs;
  for (const auto &symbol : smaller) {
    if (larger.contains(symbol)) {
      result.insert(symbol);
    }
  }
  return result;
}

bool StringEquals(const std::string &value, std::string_view expected) {
  return std::string_view(value) == expected;
}

bool StringVectorContains(const std::vector<std::string> &values,
                          std::string_view expected) {
  return std::any_of(values.begin(), values.end(),
                     [expected](const std::string &value) {
                       return StringEquals(value, expected);
                     });
}

bool StringSetContains(const std::unordered_set<std::string> &values,
                       std::string_view expected) {
  return std::any_of(values.begin(), values.end(),
                     [expected](const std::string &value) {
                       return StringEquals(value, expected);
                     });
}

bool IsPropertyPredicateKind(PredicateKind kind) {
  return kind == PredicateKind::kPropertyEquality ||
         kind == PredicateKind::kPropertyComparison;
}

std::string_view PredicateKindKey(PredicateKind kind) {
  switch (kind) {
    case PredicateKind::kGenericExpression:
      return "generic_expression";
    case PredicateKind::kNodeLabel:
      return "node_label";
    case PredicateKind::kRelationshipType:
      return "relationship_type";
    case PredicateKind::kPropertyEquality:
      return "property_equality";
    case PredicateKind::kPropertyComparison:
      return "property_comparison";
    case PredicateKind::kExistsSubquery:
      return "exists_subquery";
  }
  THROW(common::InternalError, "unknown predicate kind");
}

std::vector<std::string> SortedCopy(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  return values;
}

void AppendKeyPart(std::string *key, std::string_view part) {
  CHECK(key != nullptr, common::InternalError, "predicate key is null");
  key->append(std::to_string(part.size()));
  key->push_back(':');
  key->append(part.data(), part.size());
  key->push_back('|');
}

void AppendKeyParts(std::string *key, std::vector<std::string> parts) {
  AppendKeyPart(key, std::to_string(parts.size()));
  for (const auto &part : SortedCopy(std::move(parts))) {
    AppendKeyPart(key, part);
  }
}

std::string ExpressionKey(const ast::Expression &expression) {
  std::string key = ast::ExpressionToString(expression);
  CHECK(!key.empty(), common::InvalidArgumentError,
        "failed to stringify predicate expression");
  return key;
}

std::string PropertyComparisonRhsKey(const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  CHECK(unwrapped != nullptr, common::InvalidArgumentError,
        "property predicate expression is null");
  CHECK(unwrapped->Is(ast::ASTNodeType::kComparisonExpression),
        common::InvalidArgumentError,
        "property predicate expression is not a comparison");
  const auto *comparison = ast::CastAst<ast::ComparisonExpression>(unwrapped);
  CHECK(comparison->right != nullptr, common::InvalidArgumentError,
        "property predicate right expression is null");
  return ExpressionKey(*comparison->right);
}

std::string PredicateKey(const Predicate &predicate) {
  CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
        "predicate expression is null");
  std::string key;
  AppendKeyPart(&key, PredicateKindKey(predicate.kind));

  switch (predicate.kind) {
    case PredicateKind::kNodeLabel:
      AppendKeyPart(&key, predicate.variable);
      AppendKeyParts(&key, predicate.labels);
      break;
    case PredicateKind::kRelationshipType:
      AppendKeyPart(&key, predicate.variable);
      AppendKeyParts(&key, predicate.relationship_types);
      break;
    case PredicateKind::kPropertyEquality:
    case PredicateKind::kPropertyComparison:
      AppendKeyPart(&key, predicate.variable);
      AppendKeyPart(&key, predicate.property_key);
      AppendKeyPart(&key, predicate.comparison_op);
      AppendKeyPart(&key, PropertyComparisonRhsKey(predicate.expression));
      break;
    case PredicateKind::kGenericExpression:
    case PredicateKind::kExistsSubquery:
      AppendKeyPart(&key, ExpressionKey(*predicate.expression));
      break;
  }
  CHECK(!key.empty(), common::InternalError, "predicate key is empty");
  return key;
}

bool QueryGraphContainsRelationshipVariable(const QueryGraph *query_graph,
                                            const std::string &name) {
  if (query_graph == nullptr) {
    return false;
  }
  for (const auto &relationship : query_graph->pattern_relationships) {
    if (relationship.variable == name) {
      return true;
    }
  }
  return false;
}

bool QueryGraphContainsNodeVariable(const QueryGraph *query_graph,
                                    const std::string &name) {
  return query_graph != nullptr && query_graph->pattern_nodes.contains(name);
}

void ClassifyPredicate(Predicate *predicate,
                       const ast::SemanticTable &semantic_table,
                       const QueryGraph *query_graph) {
  CHECK(predicate != nullptr, common::InternalError, "predicate is null");
  CHECK(predicate->expression != nullptr, common::InvalidArgumentError,
        "predicate expression is null");
  const ast::Expression *expression =
      UnwrapParenthesized(predicate->expression);
  CHECK(expression != nullptr, common::InvalidArgumentError,
        "predicate expression is null");

  if (expression->Is(ast::ASTNodeType::kLabelPredicateExpression)) {
    const auto *label = ast::CastAst<ast::LabelPredicateExpression>(expression);
    const ast::Variable *variable = AsVariableExpression(label->expr.get());
    if (variable == nullptr) {
      return;
    }
    predicate->variable = variable->name;
    auto variable_type =
        semantic_table.VariableTypeAt(*expression, variable->name);
    if (!variable_type.has_value()) {
      variable_type = semantic_table.VariableType(variable->name);
    }
    const bool type_unknown =
        !variable_type.has_value() ||
        *variable_type == ast::SemanticVariableType::kUnknown;
    if (variable_type == ast::SemanticVariableType::kRelationship ||
        (type_unknown &&
         QueryGraphContainsRelationshipVariable(query_graph, variable->name))) {
      predicate->kind = PredicateKind::kRelationshipType;
      predicate->relationship_types = label->labels;
    } else if (variable_type == ast::SemanticVariableType::kNode ||
               (type_unknown &&
                QueryGraphContainsNodeVariable(query_graph, variable->name))) {
      predicate->kind = PredicateKind::kNodeLabel;
      predicate->labels = label->labels;
    }
    return;
  }

  if (expression->Is(ast::ASTNodeType::kComparisonExpression)) {
    const auto *comparison =
        ast::CastAst<ast::ComparisonExpression>(expression);
    const ast::PropertyExpression *property =
        AsPropertyExpression(comparison->left.get());
    if (property == nullptr) {
      return;
    }
    const ast::Variable *variable =
        AsVariableExpression(property->object.get());
    if (variable == nullptr) {
      return;
    }
    predicate->variable = variable->name;
    predicate->property_key = property->property_key;
    predicate->comparison_op = comparison->op;
    predicate->kind = comparison->op == "="
                          ? PredicateKind::kPropertyEquality
                          : PredicateKind::kPropertyComparison;
    return;
  }

  if (AsExistentialSubquery(expression) != nullptr) {
    predicate->kind = PredicateKind::kExistsSubquery;
  }
}

void AddSelectionPredicates(const ast::Expression *where,
                            const ast::SemanticTable &semantic_table,
                            const QueryGraph *query_graph,
                            Selections *selections,
                            std::unordered_set<std::string> *selection_keys) {
  CHECK(where != nullptr, common::InvalidArgumentError,
        "selection predicate is null");
  CHECK(selections != nullptr, common::InternalError, "selections is null");
  CHECK(selection_keys != nullptr, common::InternalError,
        "selection keys is null");
  std::vector<const ast::Expression *> predicates;
  SplitConjunctivePredicates(where, &predicates);
  CHECK(!predicates.empty(), common::InvalidArgumentError,
        "selection predicate list is empty");
  for (const ast::Expression *predicate : predicates) {
    CHECK(predicate != nullptr, common::InvalidArgumentError,
          "null selection predicate is not supported");

    Predicate selection_predicate;
    selection_predicate.expression = predicate;
    selection_predicate.dependencies =
        semantic_table.ExpressionDependencies(*predicate);
    ClassifyPredicate(&selection_predicate, semantic_table, query_graph);
    const std::string predicate_key = PredicateKey(selection_predicate);
    if (!selection_keys->insert(predicate_key).second) {
      continue;
    }
    selections->predicates.push_back(std::move(selection_predicate));
  }
}

}  // namespace

std::vector<const Predicate *> Selections::PredicatesByKind(
    PredicateKind kind) const {
  std::vector<const Predicate *> result;
  for (const auto &predicate : predicates) {
    if (predicate.kind == kind) {
      result.push_back(&predicate);
    }
  }
  return result;
}

std::vector<const Predicate *> Selections::PredicatesByVariable(
    std::string_view variable) const {
  std::vector<const Predicate *> result;
  for (const auto &predicate : predicates) {
    if (StringEquals(predicate.variable, variable)) {
      result.push_back(&predicate);
    }
  }
  return result;
}

std::vector<const Predicate *> Selections::PredicatesDependingOn(
    std::string_view symbol) const {
  std::vector<const Predicate *> result;
  for (const auto &predicate : predicates) {
    if (StringSetContains(predicate.dependencies, symbol)) {
      result.push_back(&predicate);
    }
  }
  return result;
}

std::vector<const Predicate *> Selections::NodeLabelPredicates(
    std::string_view variable) const {
  std::vector<const Predicate *> result;
  for (const auto &predicate : predicates) {
    if (predicate.kind == PredicateKind::kNodeLabel &&
        StringEquals(predicate.variable, variable)) {
      result.push_back(&predicate);
    }
  }
  return result;
}

std::vector<const Predicate *> Selections::RelationshipTypePredicates(
    std::string_view variable) const {
  std::vector<const Predicate *> result;
  for (const auto &predicate : predicates) {
    if (predicate.kind == PredicateKind::kRelationshipType &&
        StringEquals(predicate.variable, variable)) {
      result.push_back(&predicate);
    }
  }
  return result;
}

std::vector<const Predicate *> Selections::PropertyPredicates(
    std::string_view variable, std::string_view property_key) const {
  std::vector<const Predicate *> result;
  for (const auto &predicate : predicates) {
    if (IsPropertyPredicateKind(predicate.kind) &&
        StringEquals(predicate.variable, variable) &&
        StringEquals(predicate.property_key, property_key)) {
      result.push_back(&predicate);
    }
  }
  return result;
}

std::vector<const Predicate *> Selections::PropertyPredicates(
    std::string_view variable, std::string_view property_key,
    std::string_view comparison_op) const {
  std::vector<const Predicate *> result;
  for (const auto &predicate : predicates) {
    if (IsPropertyPredicateKind(predicate.kind) &&
        StringEquals(predicate.variable, variable) &&
        StringEquals(predicate.property_key, property_key) &&
        StringEquals(predicate.comparison_op, comparison_op)) {
      result.push_back(&predicate);
    }
  }
  return result;
}

bool Selections::ContainsNodeLabel(std::string_view variable,
                                   std::string_view label) const {
  for (const auto &predicate : predicates) {
    if (predicate.kind == PredicateKind::kNodeLabel &&
        StringEquals(predicate.variable, variable) &&
        StringVectorContains(predicate.labels, label)) {
      return true;
    }
  }
  return false;
}

bool Selections::ContainsRelationshipType(std::string_view variable,
                                          std::string_view type) const {
  for (const auto &predicate : predicates) {
    if (predicate.kind == PredicateKind::kRelationshipType &&
        StringEquals(predicate.variable, variable) &&
        StringVectorContains(predicate.relationship_types, type)) {
      return true;
    }
  }
  return false;
}

bool Selections::ContainsPropertyPredicate(
    std::string_view variable, std::string_view property_key) const {
  for (const auto &predicate : predicates) {
    if (IsPropertyPredicateKind(predicate.kind) &&
        StringEquals(predicate.variable, variable) &&
        StringEquals(predicate.property_key, property_key)) {
      return true;
    }
  }
  return false;
}

bool Selections::ContainsPropertyPredicate(
    std::string_view variable, std::string_view property_key,
    std::string_view comparison_op) const {
  for (const auto &predicate : predicates) {
    if (IsPropertyPredicateKind(predicate.kind) &&
        StringEquals(predicate.variable, variable) &&
        StringEquals(predicate.property_key, property_key) &&
        StringEquals(predicate.comparison_op, comparison_op)) {
      return true;
    }
  }
  return false;
}

bool QueryGraph::HasLocalWork() const {
  return !pattern_paths.empty() || !pattern_nodes.empty() ||
         !pattern_relationships.empty() || !selections.empty() ||
         !optional_matches.empty() || !hints.empty() ||
         !mutating_patterns.empty();
}

bool QueryGraph::ContainsUpdates() const {
  if (!mutating_patterns.empty()) {
    return true;
  }
  for (const auto &optional_match : optional_matches) {
    if (optional_match.ContainsUpdates()) {
      return true;
    }
  }
  return false;
}

bool QueryGraph::ReadOnly() const { return !ContainsUpdates(); }

bool QueryGraph::CouldContainRead() const {
  if (!pattern_paths.empty() || !pattern_nodes.empty() ||
      !pattern_relationships.empty() || !selections.empty()) {
    return true;
  }
  for (const auto &optional_match : optional_matches) {
    if (optional_match.CouldContainRead()) {
      return true;
    }
  }
  return false;
}

std::unordered_set<LogicalVariable> QueryGraph::CoveredIdsForPatterns() const {
  std::unordered_set<LogicalVariable> symbols = pattern_paths;
  AddSymbols(&symbols, pattern_nodes);
  for (const auto &relationship : pattern_relationships) {
    AddSymbol(&symbols, relationship.variable);
    AddSymbol(&symbols, relationship.left_node);
    AddSymbol(&symbols, relationship.right_node);
  }
  return symbols;
}

std::unordered_set<LogicalVariable>
QueryGraph::IdsWithoutOptionalMatchesOrUpdates() const {
  std::unordered_set<LogicalVariable> symbols = CoveredIdsForPatterns();
  AddSymbols(&symbols, argument_ids);
  return symbols;
}

std::unordered_set<LogicalVariable> QueryGraph::LocalAvailableSymbols() const {
  return QueryGraphLocalAvailableSymbols(*this);
}

std::unordered_set<LogicalVariable> QueryGraph::AvailableSymbols() const {
  return QueryGraphAvailableSymbols(*this);
}

std::unordered_set<LogicalVariable> QueryGraph::AllCoveredIds() const {
  std::unordered_set<LogicalVariable> symbols =
      IdsWithoutOptionalMatchesOrUpdates();
  for (const auto &optional_match : optional_matches) {
    AddSymbols(&symbols, optional_match.AllCoveredIds());
  }
  for (const auto &mutating_pattern : mutating_patterns) {
    AddSymbols(&symbols, MutatingPatternAvailableSymbols(mutating_pattern));
  }
  return symbols;
}

std::unordered_set<LogicalVariable> QueryGraph::Dependencies() const {
  std::unordered_set<LogicalVariable> dependencies = argument_ids;
  AddSymbols(&dependencies, CoveredIdsForPatterns());
  for (const auto &predicate : selections.predicates) {
    AddSymbols(&dependencies, predicate.dependencies);
  }
  for (const auto &optional_match : optional_matches) {
    AddSymbols(&dependencies, optional_match.Dependencies());
  }
  for (const auto &mutating_pattern : mutating_patterns) {
    AddSymbols(&dependencies, MutatingPatternDependencies(mutating_pattern));
  }
  return dependencies;
}

std::unordered_set<std::string> QueryGraph::LabelsOnNode(
    std::string_view variable) const {
  std::unordered_set<std::string> labels;
  for (const auto *predicate : selections.NodeLabelPredicates(variable)) {
    for (const auto &label : predicate->labels) {
      labels.insert(label);
    }
  }
  return labels;
}

std::unordered_set<std::string> QueryGraph::TypesOnRelationship(
    std::string_view variable) const {
  std::unordered_set<std::string> types;
  for (const auto &relationship : pattern_relationships) {
    if (!StringEquals(relationship.variable, variable)) {
      continue;
    }
    for (const auto &type : relationship.types) {
      types.insert(type);
    }
  }
  for (const auto *predicate :
       selections.RelationshipTypePredicates(variable)) {
    for (const auto &type : predicate->relationship_types) {
      types.insert(type);
    }
  }
  return types;
}

std::unordered_set<std::string> QueryGraph::KnownProperties(
    std::string_view variable) const {
  std::unordered_set<std::string> properties;
  for (const auto &predicate : selections.predicates) {
    if (IsPropertyPredicateKind(predicate.kind) &&
        StringEquals(predicate.variable, variable) &&
        !predicate.property_key.empty()) {
      properties.insert(predicate.property_key);
    }
  }
  return properties;
}

std::unordered_set<std::string> QueryGraph::AllPossibleLabelsOnNode(
    std::string_view variable) const {
  std::unordered_set<std::string> labels = LabelsOnNode(variable);
  for (const auto &optional_match : optional_matches) {
    AddSymbols(&labels, optional_match.AllPossibleLabelsOnNode(variable));
  }
  return labels;
}

std::unordered_set<std::string> QueryGraph::AllPossibleTypesOnRelationship(
    std::string_view variable) const {
  std::unordered_set<std::string> types = TypesOnRelationship(variable);
  for (const auto &optional_match : optional_matches) {
    AddSymbols(&types, optional_match.AllPossibleTypesOnRelationship(variable));
  }
  return types;
}

std::unordered_set<std::string> QueryGraph::AllKnownPropertiesOnIdentifier(
    std::string_view variable) const {
  std::unordered_set<std::string> properties = KnownProperties(variable);
  for (const auto &optional_match : optional_matches) {
    AddSymbols(&properties,
               optional_match.AllKnownPropertiesOnIdentifier(variable));
  }
  return properties;
}

std::vector<QueryGraphComponent> QueryGraph::ConnectedComponents() const {
  std::unordered_set<LogicalVariable> all_nodes = pattern_nodes;
  std::unordered_map<LogicalVariable, std::vector<std::size_t>>
      relationship_indices_by_node;
  for (std::size_t i = 0; i < pattern_relationships.size(); ++i) {
    const auto &relationship = pattern_relationships[i];
    AddSymbol(&all_nodes, relationship.left_node);
    AddSymbol(&all_nodes, relationship.right_node);
    if (!relationship.left_node.empty()) {
      relationship_indices_by_node[relationship.left_node].push_back(i);
    }
    if (!relationship.right_node.empty() &&
        relationship.right_node != relationship.left_node) {
      relationship_indices_by_node[relationship.right_node].push_back(i);
    }
  }

  std::vector<LogicalVariable> start_nodes(all_nodes.begin(), all_nodes.end());
  std::sort(start_nodes.begin(), start_nodes.end());

  std::unordered_set<LogicalVariable> visited_nodes;
  std::unordered_set<std::size_t> visited_relationships;
  std::vector<QueryGraphComponent> components;

  auto add_covered_id = [&](QueryGraphComponent *component,
                            const LogicalVariable &symbol) {
    CHECK(component != nullptr, common::InternalError,
          "query graph component is null");
    AddSymbol(&component->covered_ids, symbol);
    if (!symbol.empty() && argument_ids.contains(symbol)) {
      component->touches_arguments = true;
    }
  };

  auto build_component = [&](const LogicalVariable &start_node) {
    QueryGraphComponent component;
    std::vector<LogicalVariable> stack;
    stack.push_back(start_node);
    while (!stack.empty()) {
      const LogicalVariable node = stack.back();
      stack.pop_back();
      if (!visited_nodes.insert(node).second) {
        continue;
      }
      AddSymbol(&component.pattern_nodes, node);
      add_covered_id(&component, node);

      const auto found = relationship_indices_by_node.find(node);
      if (found == relationship_indices_by_node.end()) {
        continue;
      }
      for (std::size_t relationship_index : found->second) {
        if (!visited_relationships.insert(relationship_index).second) {
          continue;
        }
        component.pattern_relationship_indices.push_back(relationship_index);
        const auto &relationship = pattern_relationships[relationship_index];
        add_covered_id(&component, relationship.variable);
        if (!relationship.left_node.empty() &&
            !visited_nodes.contains(relationship.left_node)) {
          stack.push_back(relationship.left_node);
        }
        if (!relationship.right_node.empty() &&
            !visited_nodes.contains(relationship.right_node)) {
          stack.push_back(relationship.right_node);
        }
      }
    }
    std::sort(component.pattern_relationship_indices.begin(),
              component.pattern_relationship_indices.end());
    components.push_back(std::move(component));
  };

  for (const auto &node : start_nodes) {
    if (argument_ids.contains(node) && !visited_nodes.contains(node)) {
      build_component(node);
    }
  }
  for (const auto &node : start_nodes) {
    if (!visited_nodes.contains(node)) {
      build_component(node);
    }
  }

  return components;
}

class PatternConverter {
 public:
  explicit PatternConverter(QueryGraph *graph) : graph_(graph) {
    CHECK(graph_ != nullptr, common::InternalError,
          "pattern converter query graph is null");
  }

  void AddPattern(const ast::Pattern &pattern) {
    CHECK(!pattern.parts.empty(), common::InvalidArgumentError,
          Missing("pattern part"));
    for (const auto &part : pattern.parts) {
      CHECK(part != nullptr, common::InvalidArgumentError,
            Missing("pattern part"));
      AddPatternPart(*part);
    }
  }

 private:
  void AddPatternPart(const ast::PatternPart &part) {
    if (!part.variable.empty()) {
      graph_->pattern_paths.insert(part.variable);
    }
    CHECK(part.element != nullptr, common::InvalidArgumentError,
          Missing("pattern element"));
    AddPatternElement(*part.element);
  }

  void AddPatternElement(const ast::PatternElement &element) {
    if (!element.node_pattern) {
      THROW(common::InternalError, "node_pattern is null");
    }
    std::string left = AddNode(*element.node_pattern);
    for (const auto &link : element.chain) {
      CHECK(link.first != nullptr, common::InvalidArgumentError,
            Missing("relationship pattern"));
      CHECK(link.second != nullptr, common::InvalidArgumentError,
            Missing("node pattern"));
      std::string right = AddNode(*link.second);
      AddRelationship(*link.first, left, right);
      left = right;
    }
  }

  std::string AddNode(const ast::NodePattern &node) {
    CHECK(!node.variable.empty(), common::InvalidArgumentError,
          Unsupported("anonymous node"));
    CHECK(node.labels.empty(), common::InvalidArgumentError,
          Unsupported("node with labels"));
    CHECK(!node.properties, common::InvalidArgumentError,
          Unsupported("node with properties"));
    graph_->pattern_nodes.insert(node.variable);
    return node.variable;
  }

  void AddRelationship(const ast::RelationshipPattern &pattern,
                       const std::string &left, const std::string &right) {
    const ast::RelationshipDetail *detail = pattern.detail.get();
    CHECK(detail && !detail->variable.empty(), common::InvalidArgumentError,
          Unsupported("anonymous relationship"));
    CHECK(!detail->properties, common::InvalidArgumentError,
          Unsupported("relationship with properties"));

    PatternRelationship relationship;
    relationship.variable = detail->variable;
    relationship.left_node = left;
    relationship.right_node = right;
    relationship.types = detail->types;
    if (detail->range) {
      relationship.length.variable = true;
      relationship.length.min = detail->range->min;
      relationship.length.max = detail->range->max;
    }
    if (pattern.left_arrow) {
      relationship.direction = Direction::kIncoming;
    } else if (pattern.right_arrow) {
      relationship.direction = Direction::kOutgoing;
    } else {
      relationship.direction = Direction::kBoth;
    }
    graph_->pattern_relationships.push_back(std::move(relationship));
  }

  QueryGraph *graph_ = nullptr;
};

class QueryGraphBuilder {
 public:
  explicit QueryGraphBuilder(
      const ast::SemanticTable &semantic_table,
      std::unordered_set<std::string> argument_ids = {}) {
    semantic_table_ = &semantic_table;
    graph_.argument_ids = std::move(argument_ids);
  }

  void BuildReadingClause(const ast::ReadingClause &clause) {
    switch (clause.node_type) {
      case ast::ASTNodeType::kMatch:
        BuildMatch(ast::CastAst<ast::Match>(clause));
        return;
      case ast::ASTNodeType::kUnwind:
        THROW(common::InvalidArgumentError, Unsupported("UNWIND"));
      case ast::ASTNodeType::kInQueryCall:
        THROW(common::InvalidArgumentError, Unsupported("procedure call"));
      default:
        break;
    }
    THROW(common::InvalidArgumentError, Unsupported("reading clause"));
  }

  void BuildUpdatingClause(const ast::UpdatingClause &clause) {
    graph_.mutating_patterns.push_back(BuildMutatingPattern(clause));
  }

  [[nodiscard]] bool HasLocalWork() const { return graph_.HasLocalWork(); }

  void BuildMatch(const ast::Match &match) {
    if (match.optional_match) {
      BuildOptionalMatch(match);
      return;
    }
    BuildRequiredMatch(match);
  }

  void BuildOptionalMatch(const ast::Match &match) {
    QueryGraphBuilder optional_builder(SemanticTableRef());
    optional_builder.BuildRequiredMatch(match);
    graph_.optional_matches.push_back(optional_builder.Release());
  }

  void BuildRequiredMatch(const ast::Match &match) {
    if (match.optional_match) {
      CHECK(match.pattern != nullptr, common::InvalidArgumentError,
            Missing("OPTIONAL MATCH pattern"));
    } else {
      CHECK(match.pattern != nullptr, common::InvalidArgumentError,
            Missing("MATCH pattern"));
    }
    PatternConverter converter(&graph_);
    converter.AddPattern(*match.pattern);
    if (match.where) {
      AddWhere(match.where.get());
    }
  }

  void AddWhere(const ast::Expression *where) {
    AddSelectionPredicates(where, SemanticTableRef(), &graph_,
                           &graph_.selections, &where_keys_);
  }

  QueryGraph Release() { return std::move(graph_); }

 private:
  static MutatingPattern BuildMutatingPattern(
      const ast::UpdatingClause &clause) {
    MutatingPattern mutating_pattern;
    mutating_pattern.clause = &clause;
    switch (clause.node_type) {
      case ast::ASTNodeType::kCreate: {
        const auto &create = ast::CastAst<ast::Create>(clause);
        CHECK(create.pattern != nullptr, common::InvalidArgumentError,
              Missing("CREATE pattern"));
        mutating_pattern.kind = MutatingPatternKind::kCreate;
        mutating_pattern.pattern = create.pattern.get();
        return mutating_pattern;
      }
      case ast::ASTNodeType::kMerge: {
        const auto &merge = ast::CastAst<ast::Merge>(clause);
        CHECK(merge.pattern_part != nullptr, common::InvalidArgumentError,
              Missing("MERGE pattern"));
        mutating_pattern.kind = MutatingPatternKind::kMerge;
        mutating_pattern.pattern_part = merge.pattern_part.get();
        mutating_pattern.merge_actions.reserve(merge.actions.size());
        for (const auto &action : merge.actions) {
          CHECK(action.second != nullptr, common::InvalidArgumentError,
                Missing("MERGE action SET"));
          mutating_pattern.merge_actions.push_back(
              {.on_match = action.first, .set = action.second.get()});
        }
        return mutating_pattern;
      }
      case ast::ASTNodeType::kSet: {
        const auto &set = ast::CastAst<ast::Set>(clause);
        mutating_pattern.kind = MutatingPatternKind::kSet;
        mutating_pattern.set_items.reserve(set.items.size());
        for (const auto &item : set.items) {
          CHECK(item != nullptr, common::InvalidArgumentError,
                Missing("SET item"));
          mutating_pattern.set_items.push_back(item.get());
        }
        return mutating_pattern;
      }
      case ast::ASTNodeType::kDelete: {
        const auto &del = ast::CastAst<ast::Delete>(clause);
        mutating_pattern.kind = MutatingPatternKind::kDelete;
        mutating_pattern.detach = del.detach;
        mutating_pattern.delete_expressions.reserve(del.expressions.size());
        for (const auto &expression : del.expressions) {
          CHECK(expression != nullptr, common::InvalidArgumentError,
                Missing("DELETE expression"));
          mutating_pattern.delete_expressions.push_back(expression.get());
        }
        return mutating_pattern;
      }
      case ast::ASTNodeType::kRemove: {
        const auto &remove = ast::CastAst<ast::Remove>(clause);
        mutating_pattern.kind = MutatingPatternKind::kRemove;
        mutating_pattern.remove_items.reserve(remove.items.size());
        for (const auto &item : remove.items) {
          CHECK(item != nullptr, common::InvalidArgumentError,
                Missing("REMOVE item"));
          mutating_pattern.remove_items.push_back(item.get());
        }
        return mutating_pattern;
      }
      default:
        break;
    }
    THROW(common::InvalidArgumentError, Unsupported("updating clause"));
  }

  [[nodiscard]] const ast::SemanticTable &SemanticTableRef() const {
    CHECK(semantic_table_ != nullptr, common::InternalError,
          "semantic table is null");
    return *semantic_table_;
  }

  QueryGraph graph_;
  std::unordered_set<std::string> where_keys_;
  const ast::SemanticTable *semantic_table_ = nullptr;
};

QueryHorizon QueryHorizon::ForRegularProjection(
    RegularQueryProjection projection) {
  QueryHorizon horizon;
  horizon.kind = QueryHorizonKind::kRegularProjection;
  horizon.regular_projection = std::move(projection);
  return horizon;
}

QueryHorizon QueryHorizon::ForDistinctProjection(
    DistinctQueryProjection projection) {
  QueryHorizon horizon;
  horizon.kind = QueryHorizonKind::kDistinctProjection;
  horizon.distinct_projection = std::move(projection);
  return horizon;
}

QueryHorizon QueryHorizon::ForAggregatingProjection(
    AggregatingQueryProjection projection) {
  QueryHorizon horizon;
  horizon.kind = QueryHorizonKind::kAggregatingProjection;
  horizon.aggregating_projection = std::move(projection);
  return horizon;
}

QueryHorizon QueryHorizon::ForUnwind(UnwindHorizon unwind) {
  QueryHorizon horizon;
  horizon.kind = QueryHorizonKind::kUnwind;
  horizon.unwind = std::move(unwind);
  return horizon;
}

QueryHorizon QueryHorizon::ForPassthrough() {
  QueryHorizon horizon;
  horizon.kind = QueryHorizonKind::kPassthrough;
  return horizon;
}

const RegularQueryProjection &QueryHorizon::RequireRegularProjection() const {
  CHECK(kind == QueryHorizonKind::kRegularProjection,
        common::InvalidArgumentError,
        Unsupported("regular projection horizon"));
  return regular_projection;
}

RegularQueryProjection &QueryHorizon::RequireRegularProjection() {
  CHECK(kind == QueryHorizonKind::kRegularProjection,
        common::InvalidArgumentError,
        Unsupported("regular projection horizon"));
  return regular_projection;
}

const DistinctQueryProjection &QueryHorizon::RequireDistinctProjection() const {
  CHECK(kind == QueryHorizonKind::kDistinctProjection,
        common::InvalidArgumentError,
        Unsupported("distinct projection horizon"));
  return distinct_projection;
}

DistinctQueryProjection &QueryHorizon::RequireDistinctProjection() {
  CHECK(kind == QueryHorizonKind::kDistinctProjection,
        common::InvalidArgumentError,
        Unsupported("distinct projection horizon"));
  return distinct_projection;
}

const AggregatingQueryProjection &QueryHorizon::RequireAggregatingProjection()
    const {
  CHECK(kind == QueryHorizonKind::kAggregatingProjection,
        common::InvalidArgumentError,
        Unsupported("aggregating projection horizon"));
  return aggregating_projection;
}

AggregatingQueryProjection &QueryHorizon::RequireAggregatingProjection() {
  CHECK(kind == QueryHorizonKind::kAggregatingProjection,
        common::InvalidArgumentError,
        Unsupported("aggregating projection horizon"));
  return aggregating_projection;
}

const UnwindHorizon &QueryHorizon::RequireUnwind() const {
  CHECK(kind == QueryHorizonKind::kUnwind, common::InvalidArgumentError,
        Unsupported("query horizon"));
  return unwind;
}

UnwindHorizon &QueryHorizon::RequireUnwind() {
  CHECK(kind == QueryHorizonKind::kUnwind, common::InvalidArgumentError,
        Unsupported("query horizon"));
  return unwind;
}

const SinglePlannerQuery *SinglePlannerQuery::Last() const {
  return LastQueryPart(this);
}

SinglePlannerQuery *SinglePlannerQuery::Last() { return LastQueryPart(this); }

const SinglePlannerQuery &PlannerQuery::RequireSingle() const {
  const auto *query = dynamic_cast<const SinglePlannerQuery *>(this);
  CHECK(query != nullptr, common::InvalidArgumentError,
        Unsupported("non-single planner query"));
  return *query;
}

SinglePlannerQuery &PlannerQuery::RequireSingle() {
  auto *query = dynamic_cast<SinglePlannerQuery *>(this);
  CHECK(query != nullptr, common::InvalidArgumentError,
        Unsupported("non-single planner query"));
  return *query;
}

const UnionPlannerQuery &PlannerQuery::RequireUnion() const {
  const auto *query = dynamic_cast<const UnionPlannerQuery *>(this);
  CHECK(query != nullptr, common::InvalidArgumentError,
        Unsupported("non-union planner query"));
  return *query;
}

UnionPlannerQuery &PlannerQuery::RequireUnion() {
  auto *query = dynamic_cast<UnionPlannerQuery *>(this);
  CHECK(query != nullptr, common::InvalidArgumentError,
        Unsupported("non-union planner query"));
  return *query;
}

std::unique_ptr<PlannerQuery> MakeSinglePlannerQuery(
    SinglePlannerQuery single_query) {
  return std::make_unique<SinglePlannerQuery>(std::move(single_query));
}

std::unique_ptr<PlannerQuery> MakeUnionPlannerQuery(
    std::unique_ptr<PlannerQuery> lhs, SinglePlannerQuery rhs, bool all) {
  CHECK(lhs != nullptr, common::InvalidArgumentError,
        "UNION lhs planner query is null");
  auto query = std::make_unique<UnionPlannerQuery>();
  query->lhs = std::move(lhs);
  query->rhs = std::move(rhs);
  query->all = all;
  return query;
}

namespace {

class PlannerQueryBuilder {
 public:
  explicit PlannerQueryBuilder(const ast::SemanticTable &semantic_table)
      : semantic_table_(&semantic_table) {}

  std::unique_ptr<PlannerQuery> Build(const ast::Statement &statement) {
    switch (statement.node_type) {
      case ast::ASTNodeType::kRegularQuery: {
        std::unique_ptr<PlannerQuery> planner_query =
            BuildRegularQuery(ast::CastAst<ast::RegularQuery>(statement));
        FinalizePlannerQueryArguments(*planner_query);
        return planner_query;
      }
      default: {
        THROW(common::InvalidArgumentError, Unsupported("query type"));
      }
    }
  }

 private:
  [[nodiscard]] const ast::SemanticTable &SemanticTableRef() const {
    CHECK(semantic_table_ != nullptr, common::InternalError,
          "semantic table is null");
    return *semantic_table_;
  }

  std::unique_ptr<PlannerQuery> BuildRegularQuery(
      const ast::RegularQuery &query) {
    CHECK(query.single_query, common::InvalidArgumentError,
          Missing("single query"));

    std::unique_ptr<PlannerQuery> planner_query =
        MakeSinglePlannerQuery(BuildSingleQuery(*query.single_query));
    for (const auto &part : query.unions) {
      CHECK(part && part->query, common::InvalidArgumentError,
            Missing("UNION branch query"));
      planner_query = MakeUnionPlannerQuery(
          std::move(planner_query), BuildSingleQuery(*part->query), part->all);
    }

    return planner_query;
  }

  void AttachQueryGraphSubqueries(QueryGraph *query_graph) {
    CHECK(query_graph != nullptr, common::InternalError, "query graph is null");
    AttachSelectionSubqueries(&query_graph->selections);
    for (auto &optional_match : query_graph->optional_matches) {
      AttachQueryGraphSubqueries(&optional_match);
    }
  }

  void AttachQueryHorizonSubqueries(QueryHorizon *horizon) {
    CHECK(horizon != nullptr, common::InternalError, "query horizon is null");
    switch (horizon->kind) {
      case QueryHorizonKind::kRegularProjection:
        AttachSelectionSubqueries(
            &horizon->RequireRegularProjection().selections);
        return;
      case QueryHorizonKind::kDistinctProjection:
        AttachSelectionSubqueries(
            &horizon->RequireDistinctProjection().selections);
        return;
      case QueryHorizonKind::kAggregatingProjection:
        AttachSelectionSubqueries(
            &horizon->RequireAggregatingProjection().selections);
        return;
      case QueryHorizonKind::kUnwind:
      case QueryHorizonKind::kPassthrough:
        return;
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  void AttachSelectionSubqueries(Selections *selections) {
    CHECK(selections != nullptr, common::InternalError, "selections is null");
    for (auto &predicate : selections->predicates) {
      if (predicate.kind != PredicateKind::kExistsSubquery) {
        continue;
      }
      const ast::ExistentialSubquery *subquery =
          AsExistentialSubquery(predicate.expression);
      CHECK(subquery != nullptr, common::InvalidArgumentError,
            "EXISTS predicate expression is not an existential subquery");
      CHECK(subquery->query != nullptr, common::InvalidArgumentError,
            Missing("EXISTS subquery"));
      predicate.subquery = BuildRegularQuery(*subquery->query);
    }
  }

  void FinalizePlannerQueryArguments(PlannerQuery &query) const {
    FinalizePlannerQueryArguments(query, {});
  }

  void FinalizePlannerQueryArguments(
      PlannerQuery &query,
      const std::unordered_set<std::string> &available_symbols) const {
    switch (query.Kind()) {
      case PlannerQueryKind::kSingle:
        FinalizeSinglePlannerQueryArguments(&query.RequireSingle(),
                                            available_symbols);
        return;
      case PlannerQueryKind::kUnion: {
        UnionPlannerQuery &union_query = query.RequireUnion();
        CHECK(union_query.lhs != nullptr, common::InvalidArgumentError,
              "UNION lhs planner query is null");
        FinalizePlannerQueryArguments(*union_query.lhs, available_symbols);
        FinalizeSinglePlannerQueryArguments(&union_query.rhs,
                                            available_symbols);
        return;
      }
    }
    THROW(common::InternalError, "unknown planner query kind");
  }

  void FinalizeSinglePlannerQueryArguments(
      SinglePlannerQuery *query,
      std::unordered_set<std::string> available_symbols) const {
    CHECK(query != nullptr, common::InternalError, "planner query is null");
    SinglePlannerQuery *segment = query;
    while (segment != nullptr) {
      const std::unordered_set<std::string> dependencies =
          SinglePlannerQueryDependencies(*segment);
      segment->query_graph.argument_ids =
          IntersectSymbols(dependencies, available_symbols);
      FinalizeQueryGraphArguments(&segment->query_graph);
      std::unordered_set<std::string> output_symbols =
          SinglePlannerQueryOutputSymbols(*segment);
      FinalizeQueryHorizonArguments(&segment->horizon, output_symbols);
      available_symbols = std::move(output_symbols);
      segment = segment->tail.get();
    }
  }

  void FinalizeQueryGraphArguments(QueryGraph *query_graph) const {
    CHECK(query_graph != nullptr, common::InternalError, "query graph is null");
    std::unordered_set<std::string> available_symbols =
        QueryGraphLocalAvailableSymbols(*query_graph);
    FinalizeSelectionSubqueries(&query_graph->selections, available_symbols);
    for (auto &optional_match : query_graph->optional_matches) {
      const std::unordered_set<std::string> dependencies =
          QueryGraphDependencies(optional_match);
      optional_match.argument_ids =
          IntersectSymbols(dependencies, available_symbols);
      FinalizeQueryGraphArguments(&optional_match);
      const std::unordered_set<std::string> optional_symbols =
          QueryGraphAvailableSymbols(optional_match);
      AddSymbols(&available_symbols, optional_symbols);
    }
  }

  void FinalizeSelectionSubqueries(
      Selections *selections,
      const std::unordered_set<std::string> &available_symbols) const {
    CHECK(selections != nullptr, common::InternalError, "selections is null");
    for (auto &predicate : selections->predicates) {
      if (predicate.subquery == nullptr) {
        continue;
      }
      FinalizePlannerQueryArguments(*predicate.subquery, available_symbols);
    }
  }

  void FinalizeQueryHorizonArguments(
      QueryHorizon *horizon,
      const std::unordered_set<std::string> &available_symbols) const {
    CHECK(horizon != nullptr, common::InternalError, "query horizon is null");
    switch (horizon->kind) {
      case QueryHorizonKind::kRegularProjection:
        FinalizeSelectionSubqueries(
            &horizon->RequireRegularProjection().selections, available_symbols);
        return;
      case QueryHorizonKind::kDistinctProjection:
        FinalizeSelectionSubqueries(
            &horizon->RequireDistinctProjection().selections,
            available_symbols);
        return;
      case QueryHorizonKind::kAggregatingProjection:
        FinalizeSelectionSubqueries(
            &horizon->RequireAggregatingProjection().selections,
            available_symbols);
        return;
      case QueryHorizonKind::kUnwind:
      case QueryHorizonKind::kPassthrough:
        return;
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  std::unordered_set<std::string> SinglePlannerQueryDependencies(
      const SinglePlannerQuery &query) const {
    std::unordered_set<std::string> dependencies =
        QueryGraphDependencies(query.query_graph);
    AddSymbols(&dependencies, QueryHorizonDependencies(query.horizon));
    return dependencies;
  }

  std::unordered_set<std::string> QueryGraphDependencies(
      const QueryGraph &query_graph) const {
    std::unordered_set<std::string> dependencies;
    AddSymbols(&dependencies, query_graph.pattern_paths);
    AddSymbols(&dependencies, query_graph.pattern_nodes);
    for (const auto &relationship : query_graph.pattern_relationships) {
      AddSymbol(&dependencies, relationship.variable);
      AddSymbol(&dependencies, relationship.left_node);
      AddSymbol(&dependencies, relationship.right_node);
    }
    for (const auto &predicate : query_graph.selections.predicates) {
      AddSymbols(&dependencies, predicate.dependencies);
    }
    for (const auto &optional_match : query_graph.optional_matches) {
      AddSymbols(&dependencies, QueryGraphDependencies(optional_match));
    }
    for (const auto &mutating_pattern : query_graph.mutating_patterns) {
      AddMutatingPatternDependencies(&dependencies, mutating_pattern);
    }
    return dependencies;
  }

  void AddExpressionDependencies(std::unordered_set<std::string> *dependencies,
                                 const ast::Expression *expression) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    if (expression == nullptr) {
      return;
    }
    AddSymbols(dependencies,
               SemanticTableRef().ExpressionDependencies(*expression));
  }

  void AddProjectionTailDependencies(
      std::unordered_set<std::string> *dependencies,
      const QueryProjection &projection) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    for (const auto &item : projection.required_order.items) {
      AddExpressionDependencies(dependencies, item.expression);
    }
    AddSelectionDependencies(dependencies, projection.selections);
    AddExpressionDependencies(dependencies, projection.pagination.skip);
    AddExpressionDependencies(dependencies, projection.pagination.limit);
  }

  static void AddSelectionDependencies(
      std::unordered_set<std::string> *dependencies,
      const Selections &selections) {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    for (const auto &predicate : selections.predicates) {
      AddSymbols(dependencies, predicate.dependencies);
    }
  }

  void AddProjectionItemDependencies(
      std::unordered_set<std::string> *dependencies,
      const std::vector<ProjectionItem> &items) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    for (const auto &item : items) {
      AddExpressionDependencies(dependencies, item.expression);
    }
  }

  void AddPropertiesDependencies(std::unordered_set<std::string> *dependencies,
                                 const ast::Properties *properties) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    if (properties == nullptr || properties->map == nullptr) {
      return;
    }
    for (const auto &entry : properties->map->entries) {
      AddExpressionDependencies(dependencies, entry.second.get());
    }
  }

  void AddNodePatternDependencies(std::unordered_set<std::string> *dependencies,
                                  const ast::NodePattern *node) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    if (node == nullptr) {
      return;
    }
    AddSymbol(dependencies, node->variable);
    AddPropertiesDependencies(dependencies, node->properties.get());
  }

  void AddRelationshipPatternDependencies(
      std::unordered_set<std::string> *dependencies,
      const ast::RelationshipPattern *relationship) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    if (relationship == nullptr || relationship->detail == nullptr) {
      return;
    }
    AddSymbol(dependencies, relationship->detail->variable);
    AddPropertiesDependencies(dependencies,
                              relationship->detail->properties.get());
  }

  void AddPatternElementDependencies(
      std::unordered_set<std::string> *dependencies,
      const ast::PatternElement *element) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    if (element == nullptr) {
      return;
    }
    AddNodePatternDependencies(dependencies, element->node_pattern.get());
    for (const auto &link : element->chain) {
      AddRelationshipPatternDependencies(dependencies, link.first.get());
      AddNodePatternDependencies(dependencies, link.second.get());
    }
  }

  void AddPatternPartDependencies(std::unordered_set<std::string> *dependencies,
                                  const ast::PatternPart *part) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    if (part == nullptr) {
      return;
    }
    AddSymbol(dependencies, part->variable);
    AddPatternElementDependencies(dependencies, part->element.get());
  }

  void AddPatternDependencies(std::unordered_set<std::string> *dependencies,
                              const ast::Pattern *pattern) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    if (pattern == nullptr) {
      return;
    }
    for (const auto &part : pattern->parts) {
      AddPatternPartDependencies(dependencies, part.get());
    }
  }

  void AddSetDependencies(std::unordered_set<std::string> *dependencies,
                          const ast::Set *set) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    if (set == nullptr) {
      return;
    }
    for (const auto &item : set->items) {
      if (item == nullptr) {
        continue;
      }
      AddExpressionDependencies(dependencies, item->target.get());
      AddExpressionDependencies(dependencies, item->value.get());
    }
  }

  void AddMutatingPatternDependencies(
      std::unordered_set<std::string> *dependencies,
      const MutatingPattern &mutating_pattern) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    switch (mutating_pattern.kind) {
      case MutatingPatternKind::kCreate:
        AddPatternDependencies(dependencies, mutating_pattern.pattern);
        return;
      case MutatingPatternKind::kMerge:
        AddPatternPartDependencies(dependencies, mutating_pattern.pattern_part);
        for (const auto &action : mutating_pattern.merge_actions) {
          AddSetDependencies(dependencies, action.set);
        }
        return;
      case MutatingPatternKind::kSet:
        for (const auto *item : mutating_pattern.set_items) {
          if (item == nullptr) {
            continue;
          }
          AddExpressionDependencies(dependencies, item->target.get());
          AddExpressionDependencies(dependencies, item->value.get());
        }
        return;
      case MutatingPatternKind::kDelete:
        for (const auto *expression : mutating_pattern.delete_expressions) {
          AddExpressionDependencies(dependencies, expression);
        }
        return;
      case MutatingPatternKind::kRemove:
        for (const auto *item : mutating_pattern.remove_items) {
          if (item == nullptr) {
            continue;
          }
          AddExpressionDependencies(dependencies, item->target.get());
        }
        return;
    }
    THROW(common::InternalError, "unknown mutating pattern kind");
  }

  std::unordered_set<std::string> QueryHorizonDependencies(
      const QueryHorizon &horizon) const {
    std::unordered_set<std::string> dependencies;
    switch (horizon.kind) {
      case QueryHorizonKind::kRegularProjection: {
        const RegularQueryProjection &projection =
            horizon.RequireRegularProjection();
        AddProjectionItemDependencies(&dependencies, projection.items);
        AddProjectionTailDependencies(&dependencies, projection);
        return dependencies;
      }
      case QueryHorizonKind::kDistinctProjection: {
        const DistinctQueryProjection &projection =
            horizon.RequireDistinctProjection();
        AddProjectionItemDependencies(&dependencies, projection.grouping_items);
        AddProjectionTailDependencies(&dependencies, projection);
        return dependencies;
      }
      case QueryHorizonKind::kAggregatingProjection: {
        const AggregatingQueryProjection &projection =
            horizon.RequireAggregatingProjection();
        AddProjectionItemDependencies(&dependencies, projection.grouping_items);
        AddProjectionItemDependencies(&dependencies,
                                      projection.aggregation_items);
        AddProjectionTailDependencies(&dependencies, projection);
        return dependencies;
      }
      case QueryHorizonKind::kUnwind:
        AddExpressionDependencies(&dependencies,
                                  horizon.RequireUnwind().expression);
        return dependencies;
      case QueryHorizonKind::kPassthrough:
        return dependencies;
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  std::unordered_set<std::string> SinglePlannerQueryOutputSymbols(
      const SinglePlannerQuery &query) const {
    switch (query.horizon.kind) {
      case QueryHorizonKind::kRegularProjection:
        return ProjectionItemAliases(
            query.horizon.RequireRegularProjection().items);
      case QueryHorizonKind::kDistinctProjection:
        return ProjectionItemAliases(
            query.horizon.RequireDistinctProjection().grouping_items);
      case QueryHorizonKind::kAggregatingProjection: {
        std::unordered_set<std::string> symbols = ProjectionItemAliases(
            query.horizon.RequireAggregatingProjection().grouping_items);
        AddSymbols(&symbols, ProjectionItemAliases(
                                 query.horizon.RequireAggregatingProjection()
                                     .aggregation_items));
        return symbols;
      }
      case QueryHorizonKind::kUnwind: {
        std::unordered_set<std::string> symbols =
            QueryGraphAvailableSymbols(query.query_graph);
        AddSymbol(&symbols, query.horizon.RequireUnwind().alias);
        return symbols;
      }
      case QueryHorizonKind::kPassthrough:
        return QueryGraphAvailableSymbols(query.query_graph);
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  static std::unordered_set<std::string> ProjectionItemAliases(
      const std::vector<ProjectionItem> &items) {
    std::unordered_set<std::string> symbols;
    for (const auto &item : items) {
      AddSymbol(&symbols, item.alias);
    }
    return symbols;
  }

  SinglePlannerQuery BuildSingleQuery(const ast::SingleQuery &query) {
    switch (query.node_type) {
      case ast::ASTNodeType::kSinglePartQuery: {
        return BuildSinglePartQuery(ast::CastAst<ast::SinglePartQuery>(query));
      }
      case ast::ASTNodeType::kMultiPartQuery: {
        return BuildMultiPartQuery(ast::CastAst<ast::MultiPartQuery>(query));
      }
      default: {
        THROW(common::InvalidArgumentError, Unsupported("single query type"));
      }
    }
  }

  SinglePlannerQuery BuildSinglePartQuery(const ast::SinglePartQuery &query) {
    CHECK(query.return_clause || !query.updating_clauses.empty(),
          common::InvalidArgumentError, Missing("RETURN clause"));
    return BuildQuerySegment(query.reading_clauses, query.updating_clauses,
                             query.return_clause.get(), {});
  }

  SinglePlannerQuery BuildMultiPartQuery(const ast::MultiPartQuery &query) {
    CHECK(query.final_single_part_query, common::InvalidArgumentError,
          Missing("final single query"));

    SinglePlannerQuery root;
    SinglePlannerQuery *current_segment = &root;
    std::unordered_set<std::string> argument_ids;
    for (const auto &part : query.parts) {
      CHECK(part.with_clause, common::InvalidArgumentError,
            Missing("WITH clause"));
      *current_segment =
          BuildQuerySegment(part.reading_clauses, part.updating_clauses,
                            part.with_clause.get(), argument_ids);
      current_segment = current_segment->Last();
      CHECK(part.with_clause->body != nullptr, common::InvalidArgumentError,
            Missing("WITH body"));
      argument_ids =
          ProjectionOutputSymbols(SemanticTableRef(), *part.with_clause->body);
      current_segment->tail = std::make_unique<SinglePlannerQuery>();
      current_segment = current_segment->tail.get();
    }

    *current_segment =
        BuildSinglePartQuery(*query.final_single_part_query, argument_ids);
    return root;
  }

  SinglePlannerQuery BuildSinglePartQuery(
      const ast::SinglePartQuery &query,
      const std::unordered_set<std::string> &argument_ids) {
    CHECK(query.return_clause || !query.updating_clauses.empty(),
          common::InvalidArgumentError, Missing("RETURN clause"));
    return BuildQuerySegment(query.reading_clauses, query.updating_clauses,
                             query.return_clause.get(), argument_ids);
  }

  struct ProjectionParts {
    bool distinct = false;
    std::vector<ProjectionItem> items;
    std::vector<ProjectionItem> grouping_items;
    std::vector<ProjectionItem> aggregation_items;
    RequiredOrder required_order;
    const ast::Expression *skip = nullptr;
    const ast::Expression *limit = nullptr;
  };

  static void MoveProjectionTail(ProjectionParts *parts,
                                 QueryProjection *projection) {
    CHECK(parts != nullptr, common::InternalError, "projection parts is null");
    CHECK(projection != nullptr, common::InternalError, "projection is null");
    projection->required_order = std::move(parts->required_order);
    projection->pagination.skip = parts->skip;
    projection->pagination.limit = parts->limit;
  }

  void AddProjectionSelections(QueryHorizon *horizon,
                               const ast::Expression *where) {
    CHECK(horizon != nullptr, common::InternalError, "query horizon is null");
    if (where == nullptr) {
      return;
    }
    std::unordered_set<std::string> selection_keys;
    switch (horizon->kind) {
      case QueryHorizonKind::kRegularProjection:
        AddSelectionPredicates(where, SemanticTableRef(), nullptr,
                               &horizon->RequireRegularProjection().selections,
                               &selection_keys);
        return;
      case QueryHorizonKind::kDistinctProjection:
        AddSelectionPredicates(where, SemanticTableRef(), nullptr,
                               &horizon->RequireDistinctProjection().selections,
                               &selection_keys);
        return;
      case QueryHorizonKind::kAggregatingProjection:
        AddSelectionPredicates(
            where, SemanticTableRef(), nullptr,
            &horizon->RequireAggregatingProjection().selections,
            &selection_keys);
        return;
      case QueryHorizonKind::kUnwind:
        THROW(common::InternalError,
              "UNWIND horizon cannot have projection WHERE");
      case QueryHorizonKind::kPassthrough:
        THROW(common::InternalError,
              "passthrough horizon cannot have projection WHERE");
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  QueryHorizon BuildProjectionClause(
      const ast::ProjectionClause *projection_clause) {
    CHECK(projection_clause != nullptr, common::InvalidArgumentError,
          Missing("projection clause"));
    CHECK(projection_clause->body, common::InvalidArgumentError,
          Missing("projection body"));
    QueryHorizon horizon = BuildProjectionBody(*projection_clause->body);
    if (projection_clause->Is(ast::ASTNodeType::kWith)) {
      const auto *with_clause = ast::CastAst<ast::With>(projection_clause);
      AddProjectionSelections(&horizon, with_clause->where.get());
    }
    AttachQueryHorizonSubqueries(&horizon);
    return horizon;
  }

  SinglePlannerQuery BuildQuerySegment(
      const std::vector<std::unique_ptr<ast::ReadingClause>> &reading,
      const std::vector<std::unique_ptr<ast::UpdatingClause>> &updating,
      const ast::ProjectionClause *projection,
      const std::unordered_set<std::string> &argument_ids) {
    CHECK(projection != nullptr || !updating.empty(),
          common::InvalidArgumentError, Missing("projection clause"));
    SinglePlannerQuery root;
    SinglePlannerQuery *current_segment = &root;
    std::unordered_set<std::string> current_argument_ids = argument_ids;
    QueryGraphBuilder builder(SemanticTableRef(), current_argument_ids);
    bool current_segment_finished = false;

    auto finish_current_segment = [&](QueryHorizon horizon) {
      current_segment->query_graph = builder.Release();
      AttachQueryGraphSubqueries(&current_segment->query_graph);
      current_segment->horizon = std::move(horizon);
      current_segment_finished = true;
    };

    auto start_next_segment = [&]() {
      current_argument_ids = SinglePlannerQueryOutputSymbols(*current_segment);
      current_segment->tail = std::make_unique<SinglePlannerQuery>();
      current_segment = current_segment->tail.get();
      builder = QueryGraphBuilder(SemanticTableRef(), current_argument_ids);
      current_segment_finished = false;
    };

    auto finish_and_start_next = [&](QueryHorizon horizon) {
      finish_current_segment(std::move(horizon));
      start_next_segment();
    };

    for (const auto &clause : reading) {
      CHECK(clause != nullptr, common::InvalidArgumentError,
            Missing("reading clause"));
      if (clause->Is(ast::ASTNodeType::kUnwind)) {
        finish_and_start_next(QueryHorizon::ForUnwind(
            BuildUnwindHorizon(*ast::CastAst<ast::Unwind>(clause.get()))));
        continue;
      }
      builder.BuildReadingClause(*clause);
    }

    for (std::size_t i = 0; i < updating.size(); ++i) {
      const auto &clause = updating[i];
      CHECK(clause != nullptr, common::InvalidArgumentError,
            Missing("updating clause"));
      const bool is_merge = clause->Is(ast::ASTNodeType::kMerge);
      if (is_merge && builder.HasLocalWork()) {
        finish_and_start_next(QueryHorizon::ForPassthrough());
      }
      builder.BuildUpdatingClause(*clause);
      if (is_merge) {
        finish_current_segment(QueryHorizon::ForPassthrough());
        if (i + 1 < updating.size() || projection != nullptr) {
          start_next_segment();
        }
      }
    }

    if (projection != nullptr) {
      finish_current_segment(BuildProjectionClause(projection));
    } else if (!current_segment_finished && builder.HasLocalWork()) {
      finish_current_segment(QueryHorizon::ForPassthrough());
    }
    return root;
  }

  QueryHorizon BuildProjectionBody(const ast::ProjectionBody &body) {
    CHECK(!body.star, common::InvalidArgumentError,
          Unsupported("projection star before rewrite"));

    ProjectionParts parts;
    parts.distinct = body.distinct;

    parts.items.reserve(body.items.size());
    parts.grouping_items.reserve(body.items.size());
    parts.aggregation_items.reserve(body.items.size());
    for (const auto &item : body.items) {
      CHECK(item, common::InvalidArgumentError,
            "null projection item is not supported");
      CHECK(item->expression, common::InvalidArgumentError,
            Missing("projection item expression"));
      ProjectionItem projection_item;
      projection_item.expression = item->expression.get();
      projection_item.alias = item->alias;
      CHECK(!projection_item.alias.empty(), common::InvalidArgumentError,
            "projection item alias is empty after rewrite");
      parts.items.push_back(std::move(projection_item));
      const ProjectionItem &stored_item = parts.items.back();
      if (SemanticTableRef().ContainsAggregation(*stored_item.expression)) {
        parts.aggregation_items.push_back(stored_item);
      } else {
        parts.grouping_items.push_back(stored_item);
      }
    }

    parts.required_order.items.reserve(body.order_by.size());
    for (const auto &item : body.order_by) {
      CHECK(item, common::InvalidArgumentError,
            "null sort item is not supported");
      CHECK(item->expression, common::InvalidArgumentError,
            Missing("sort expression"));
      OrderItem order_item;
      order_item.expression = item->expression.get();
      order_item.direction = item->ascending ? OrderDirection::kAscending
                                             : OrderDirection::kDescending;
      parts.required_order.items.emplace_back(order_item);
    }

    parts.skip = body.skip.get();
    parts.limit = body.limit.get();

    if (!parts.aggregation_items.empty()) {
      AggregatingQueryProjection projection;
      projection.grouping_items = std::move(parts.grouping_items);
      projection.aggregation_items = std::move(parts.aggregation_items);
      MoveProjectionTail(&parts, &projection);
      return QueryHorizon::ForAggregatingProjection(std::move(projection));
    }

    if (parts.distinct) {
      DistinctQueryProjection projection;
      projection.grouping_items = std::move(parts.items);
      MoveProjectionTail(&parts, &projection);
      return QueryHorizon::ForDistinctProjection(std::move(projection));
    }

    RegularQueryProjection projection;
    projection.items = std::move(parts.items);
    MoveProjectionTail(&parts, &projection);
    return QueryHorizon::ForRegularProjection(std::move(projection));
  }

  static UnwindHorizon BuildUnwindHorizon(const ast::Unwind &unwind) {
    CHECK(unwind.expression != nullptr, common::InvalidArgumentError,
          Missing("UNWIND expression"));
    CHECK(!unwind.variable.empty(), common::InvalidArgumentError,
          Missing("UNWIND variable"));

    UnwindHorizon horizon;
    horizon.expression = unwind.expression.get();
    horizon.alias = unwind.variable;
    return horizon;
  }

  const ast::SemanticTable *semantic_table_ = nullptr;
};

}  // namespace

std::unique_ptr<PlannerQuery> CreatePlannerQuery(
    const ast::Statement &statement) {
  const ast::SemanticTable semantic_table =
      ast::AnalyzeSemanticTable(statement);
  PlannerQueryBuilder builder(semantic_table);
  return builder.Build(statement);
}

}  // namespace ir
