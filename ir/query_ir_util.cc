#include <algorithm>
#include <iterator>
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
#include "ir/query_ir_internal.h"

namespace ir {

std::string Unsupported(std::string_view feature) {
  return std::string(feature) + " is not supported";
}

std::string UnsupportedInStage(std::string_view stage,
                               std::string_view feature) {
  return std::string(stage) + ": " + Unsupported(feature);
}

std::string Missing(std::string_view subject) {
  return "missing " + std::string(subject);
}

void SplitConjunctivePredicates(const ast::Expression *expression,
                                std::vector<const ast::Expression *> *output) {
  RG_CHECK(output != nullptr, common::ErrorCode::InternalError,
           "predicate output is null");
  RG_CHECK(expression != nullptr, common::ErrorCode::InvalidParameter,
           "null WHERE predicate is not supported");
  if (expression->Is(ast::ASTNodeType::kAndExpression)) {
    const auto *and_expression = ast::CastAst<ast::AndExpression>(expression);
    SplitConjunctivePredicates(and_expression->left.get(), output);
    SplitConjunctivePredicates(and_expression->right.get(), output);
    return;
  }
  output->push_back(expression);
}

SingleQueryIR *LastQueryPart(SingleQueryIR *query) {
  SingleQueryIR *current = query;
  while (current->tail) {
    current = current->tail.get();
  }
  return current;
}

const SingleQueryIR *LastQueryPart(const SingleQueryIR *query) {
  const SingleQueryIR *current = query;
  while (current->tail) {
    current = current->tail.get();
  }
  return current;
}

const ast::Variable *AsVariableExpression(const ast::Expression *expression) {
  if (expression == nullptr || !expression->Is(ast::ASTNodeType::kVariable)) {
    return nullptr;
  }
  return ast::CastAst<ast::Variable>(expression);
}

const ast::PropertyExpression *AsPropertyExpression(
    const ast::Expression *expression) {
  if (expression == nullptr ||
      !expression->Is(ast::ASTNodeType::kPropertyExpression)) {
    return nullptr;
  }
  return ast::CastAst<ast::PropertyExpression>(expression);
}

const ast::ExistentialSubquery *AsExistentialSubquery(
    const ast::Expression *expression) {
  if (expression == nullptr ||
      !expression->Is(ast::ASTNodeType::kExistentialSubquery)) {
    return nullptr;
  }
  return ast::CastAst<ast::ExistentialSubquery>(expression);
}

const ast::PatternPredicateExpression *AsPatternPredicateExpression(
    const ast::Expression *expression) {
  if (expression == nullptr ||
      !expression->Is(ast::ASTNodeType::kPatternPredicateExpression)) {
    return nullptr;
  }
  return ast::CastAst<ast::PatternPredicateExpression>(expression);
}

PatternPropertyMap BuildPropertyMap(const ast::Properties *properties) {
  PatternPropertyMap out;
  if (properties == nullptr) {
    return out;
  }
  if (properties->map != nullptr) {
    out.entries.reserve(properties->map->entries.size());
    for (const auto &entry : properties->map->entries) {
      out.entries.push_back({.key = entry.first, .value = entry.second.get()});
    }
    return out;
  }
  out.parameter = properties->parameter.get();
  return out;
}

void AddCreatePatternSymbols(std::unordered_set<std::string> *symbols,
                             const CreatePattern &pattern) {
  RG_CHECK(symbols != nullptr, common::ErrorCode::InternalError,
           "symbol set is null");
  symbols->insert(pattern.path_variables.begin(), pattern.path_variables.end());
  for (const auto &node : pattern.nodes) {
    if (!node.variable.empty()) {
      symbols->insert(node.variable);
    }
  }
  for (const auto &relationship : pattern.relationships) {
    if (!relationship.variable.empty()) {
      symbols->insert(relationship.variable);
    }
    if (!relationship.left_node.empty()) {
      symbols->insert(relationship.left_node);
    }
    if (!relationship.right_node.empty()) {
      symbols->insert(relationship.right_node);
    }
  }
}

void AddCreatePatternNodeSymbols(std::unordered_set<std::string> *symbols,
                                 const CreatePattern &pattern) {
  RG_CHECK(symbols != nullptr, common::ErrorCode::InternalError,
           "symbol set is null");
  for (const auto &node : pattern.nodes) {
    if (!node.variable.empty()) {
      symbols->insert(node.variable);
    }
  }
}

std::unordered_set<std::string> CreatePatternSymbols(
    const CreatePattern &pattern) {
  std::unordered_set<std::string> symbols;
  AddCreatePatternSymbols(&symbols, pattern);
  return symbols;
}

std::unordered_set<std::string> MutatingPatternAvailableSymbols(
    const MutatingPattern &mutating_pattern) {
  switch (mutating_pattern.kind) {
    case MutatingPatternKind::kCreate:
      return CreatePatternSymbols(mutating_pattern.create);
    case MutatingPatternKind::kMerge:
      return CreatePatternSymbols(mutating_pattern.merge.create_pattern);
    case MutatingPatternKind::kSet:
    case MutatingPatternKind::kDelete:
    case MutatingPatternKind::kRemove:
      return {};
  }
  RG_THROW(common::ErrorCode::InternalError, "unknown mutating pattern kind");
}

std::unordered_set<std::string> QueryGraphLocalAvailableSymbols(
    const QueryGraph &query_graph) {
  std::unordered_set<std::string> symbols = query_graph.argument_ids;
  symbols.insert(query_graph.path_variables.begin(),
                 query_graph.path_variables.end());
  symbols.insert(query_graph.pattern_nodes.begin(),
                 query_graph.pattern_nodes.end());
  for (const auto &relationship : query_graph.pattern_relationships) {
    RG_CHECK(!relationship.variable.empty(),
             common::ErrorCode::InvalidParameter,
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

void AddSymbol(std::unordered_set<std::string> *symbols,
               const std::string &symbol) {
  RG_CHECK(symbols != nullptr, common::ErrorCode::InternalError,
           "symbol set is null");
  if (!symbol.empty()) {
    symbols->insert(symbol);
  }
}

void AddSymbols(std::unordered_set<std::string> *symbols,
                const std::unordered_set<std::string> &incoming) {
  RG_CHECK(symbols != nullptr, common::ErrorCode::InternalError,
           "symbol set is null");
  symbols->insert(incoming.begin(), incoming.end());
}

void AddExpressionDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const ast::Expression *expression) {
  RG_CHECK(dependencies != nullptr, common::ErrorCode::InternalError,
           "dependency set is null");
  if (expression == nullptr) {
    return;
  }
  AddSymbols(dependencies, ast::CollectExpressionDependencies(*expression));
}

void AddPropertyMapDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const PatternPropertyMap &properties) {
  RG_CHECK(dependencies != nullptr, common::ErrorCode::InternalError,
           "dependency set is null");
  for (const auto &entry : properties.entries) {
    AddExpressionDependencySymbols(dependencies, entry.value);
  }
  AddExpressionDependencySymbols(dependencies, properties.parameter);
}

void AddCreatePatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const CreatePattern &pattern) {
  RG_CHECK(dependencies != nullptr, common::ErrorCode::InternalError,
           "dependency set is null");
  AddCreatePatternSymbols(dependencies, pattern);
  for (const auto &node : pattern.nodes) {
    AddPropertyMapDependencySymbols(dependencies, node.properties);
  }
  for (const auto &relationship : pattern.relationships) {
    AddPropertyMapDependencySymbols(dependencies, relationship.properties);
  }
}

void AddSetMutatingPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const SetMutatingPattern &pattern) {
  RG_CHECK(dependencies != nullptr, common::ErrorCode::InternalError,
           "dependency set is null");
  AddExpressionDependencySymbols(dependencies, pattern.entity);
  AddExpressionDependencySymbols(dependencies, pattern.value);
}

void AddSetMutatingPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const std::vector<SetMutatingPattern> &patterns) {
  RG_CHECK(dependencies != nullptr, common::ErrorCode::InternalError,
           "dependency set is null");
  for (const auto &pattern : patterns) {
    AddSetMutatingPatternDependencySymbols(dependencies, pattern);
  }
}

void AddRemoveMutatingPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const RemoveMutatingPattern &pattern) {
  RG_CHECK(dependencies != nullptr, common::ErrorCode::InternalError,
           "dependency set is null");
  AddExpressionDependencySymbols(dependencies, pattern.entity);
}

void AddRemoveMutatingPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const std::vector<RemoveMutatingPattern> &patterns) {
  RG_CHECK(dependencies != nullptr, common::ErrorCode::InternalError,
           "dependency set is null");
  for (const auto &pattern : patterns) {
    AddRemoveMutatingPatternDependencySymbols(dependencies, pattern);
  }
}

std::unordered_set<std::string> MutatingPatternDependencies(
    const MutatingPattern &mutating_pattern) {
  std::unordered_set<std::string> dependencies;
  switch (mutating_pattern.kind) {
    case MutatingPatternKind::kCreate:
      AddCreatePatternDependencySymbols(&dependencies, mutating_pattern.create);
      break;
    case MutatingPatternKind::kMerge:
      AddCreatePatternDependencySymbols(&dependencies,
                                        mutating_pattern.merge.create_pattern);
      for (const auto &action : mutating_pattern.merge.actions) {
        AddSetMutatingPatternDependencySymbols(&dependencies,
                                               action.set_patterns);
      }
      break;
    case MutatingPatternKind::kSet:
      AddSetMutatingPatternDependencySymbols(&dependencies,
                                             mutating_pattern.set_patterns);
      break;
    case MutatingPatternKind::kDelete:
      for (const auto &pattern : mutating_pattern.delete_patterns) {
        AddExpressionDependencySymbols(&dependencies, pattern.expression);
      }
      break;
    case MutatingPatternKind::kRemove:
      AddRemoveMutatingPatternDependencySymbols(
          &dependencies, mutating_pattern.remove_patterns);
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

bool StringSetEquals(const std::unordered_set<std::string> &lhs,
                     const std::unordered_set<std::string> &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (const auto &value : lhs) {
    if (!rhs.contains(value)) {
      return false;
    }
  }
  return true;
}

bool IsLowerBoundComparison(std::string_view op) {
  return op == ">" || op == ">=";
}

bool IsUpperBoundComparison(std::string_view op) {
  return op == "<" || op == "<=";
}

bool DependenciesMet(const std::unordered_set<std::string> &dependencies,
                     const std::unordered_set<std::string> &bound_symbols) {
  for (const auto &dependency : dependencies) {
    if (!bound_symbols.contains(dependency)) {
      return false;
    }
  }
  return true;
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
    case PredicateKind::kPropertyIn:
      return "property_in";
    case PredicateKind::kPropertyStringPredicate:
      return "property_string_predicate";
    case PredicateKind::kPropertyIsNull:
      return "property_is_null";
    case PredicateKind::kPropertyIsNotNull:
      return "property_is_not_null";
    case PredicateKind::kExistsSubquery:
      return "exists_subquery";
    case PredicateKind::kNotExistsSubquery:
      return "not_exists_subquery";
  }
  RG_THROW(common::ErrorCode::InternalError, "unknown predicate kind");
}

std::vector<std::string> SortedCopy(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  return values;
}

void AppendKeyPart(std::string *key, std::string_view part) {
  RG_CHECK(key != nullptr, common::ErrorCode::InternalError,
           "predicate key is null");
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
  RG_CHECK(!key.empty(), common::ErrorCode::InvalidParameter,
           "failed to stringify predicate expression");
  return key;
}

std::string PredicateKey(const Predicate &predicate) {
  RG_CHECK(predicate.expression != nullptr, common::ErrorCode::InvalidParameter,
           "predicate expression is null");
  std::string key;
  AppendKeyPart(&key, PredicateKindKey(predicate.Kind()));

  if (const auto *label = predicate.GetIf<NodeLabelPredicate>()) {
    AppendKeyPart(&key, label->variable);
    AppendKeyParts(&key, label->labels);
  } else if (const auto *type = predicate.GetIf<RelationshipTypePredicate>()) {
    AppendKeyPart(&key, type->variable);
    AppendKeyParts(&key, type->types);
  } else if (const auto *property = predicate.GetIf<PropertyPredicate>()) {
    AppendKeyPart(&key, property->variable);
    AppendKeyPart(&key, property->property_key);
    if (property->GetIf<PropertyNullPredicate>() == nullptr) {
      AppendKeyPart(&key, property->Operator());
      RG_CHECK(property->Value() != nullptr,
               common::ErrorCode::InvalidParameter,
               "property predicate value is null");
      AppendKeyPart(&key, ExpressionKey(*property->Value()));
    }
  } else {
    AppendKeyPart(&key, ExpressionKey(*predicate.expression));
  }
  RG_CHECK(!key.empty(), common::ErrorCode::InternalError,
           "predicate key is empty");
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

bool QueryGraphRelationshipCoversVariable(const QueryGraph &query_graph,
                                          const std::string &name) {
  for (const auto &relationship : query_graph.pattern_relationships) {
    if (relationship.variable == name || relationship.left_node == name ||
        relationship.right_node == name) {
      return true;
    }
  }
  return false;
}

std::string ReverseComparisonOp(std::string_view op) {
  if (op == "<") {
    return ">";
  }
  if (op == "<=") {
    return ">=";
  }
  if (op == ">") {
    return "<";
  }
  if (op == ">=") {
    return "<=";
  }
  return std::string(op);
}

std::unique_ptr<Predicate> MakePropertyPredicate(
    std::unique_ptr<PropertyPredicate> predicate,
    const ast::PropertyExpression *property) {
  if (property == nullptr) {
    return std::make_unique<GenericPredicate>();
  }
  const ast::Variable *variable = AsVariableExpression(property->object.get());
  if (variable == nullptr || property->property_key.empty()) {
    return std::make_unique<GenericPredicate>();
  }
  predicate->variable = variable->name;
  predicate->property_key = property->property_key;
  return predicate;
}

void AddAssertIsNodeVariables(QueryGraph *query_graph) {
  RG_CHECK(query_graph != nullptr, common::ErrorCode::InternalError,
           "query graph is null");
  for (const auto &node : query_graph->pattern_nodes) {
    if (query_graph->argument_ids.contains(node) &&
        !QueryGraphRelationshipCoversVariable(*query_graph, node)) {
      query_graph->assert_is_node_variables.insert(node);
    }
  }
}

std::unique_ptr<Predicate> BuildPredicate(
    const ast::Expression *expression, const ast::SemanticTable &semantic_table,
    const QueryGraph *query_graph) {
  RG_CHECK(expression != nullptr, common::ErrorCode::InvalidParameter,
           "predicate expression is null");
  std::unique_ptr<Predicate> predicate;

  if (expression->Is(ast::ASTNodeType::kLabelPredicateExpression)) {
    const auto *label = ast::CastAst<ast::LabelPredicateExpression>(expression);
    const ast::Variable *variable = AsVariableExpression(label->expr.get());
    if (variable != nullptr) {
      auto variable_type =
          semantic_table.VariableTypeAt(*expression, variable->name);
      if (!variable_type.has_value()) {
        variable_type = semantic_table.VariableType(variable->name);
      }
      const bool type_unknown =
          !variable_type.has_value() ||
          *variable_type == ast::SemanticVariableType::kUnknown;
      if (variable_type == ast::SemanticVariableType::kRelationship ||
          (type_unknown && QueryGraphContainsRelationshipVariable(
                               query_graph, variable->name))) {
        auto type = std::make_unique<RelationshipTypePredicate>();
        type->variable = variable->name;
        type->types = label->labels;
        predicate = std::move(type);
      } else if (variable_type == ast::SemanticVariableType::kNode ||
                 (type_unknown && QueryGraphContainsNodeVariable(
                                      query_graph, variable->name))) {
        auto node_label = std::make_unique<NodeLabelPredicate>();
        node_label->variable = variable->name;
        node_label->labels = label->labels;
        predicate = std::move(node_label);
      } else {
        auto generic = std::make_unique<GenericPredicate>();
        generic->variable = variable->name;
        predicate = std::move(generic);
      }
    }
  } else if (expression->Is(ast::ASTNodeType::kComparisonExpression)) {
    const auto *comparison =
        ast::CastAst<ast::ComparisonExpression>(expression);
    const ast::PropertyExpression *property =
        AsPropertyExpression(comparison->left.get());
    const ast::Expression *value = comparison->right.get();
    std::string comparison_op = comparison->op;
    if (property == nullptr) {
      property = AsPropertyExpression(comparison->right.get());
      value = comparison->left.get();
      comparison_op = ReverseComparisonOp(comparison->op);
    }
    auto property_comparison = std::make_unique<PropertyComparisonPredicate>();
    property_comparison->op = std::move(comparison_op);
    property_comparison->value = value;
    predicate = MakePropertyPredicate(std::move(property_comparison), property);
  } else if (expression->Is(ast::ASTNodeType::kListPredicateExpression)) {
    const auto *list_predicate =
        ast::CastAst<ast::ListPredicateExpression>(expression);
    auto property_in = std::make_unique<PropertyInPredicate>();
    property_in->list = list_predicate->list.get();
    predicate = MakePropertyPredicate(
        std::move(property_in),
        AsPropertyExpression(list_predicate->element.get()));
  } else if (expression->Is(ast::ASTNodeType::kStringPredicateExpression)) {
    const auto *string_predicate =
        ast::CastAst<ast::StringPredicateExpression>(expression);
    auto property_string = std::make_unique<PropertyStringPredicate>();
    property_string->op = string_predicate->op;
    property_string->value = string_predicate->right.get();
    predicate = MakePropertyPredicate(
        std::move(property_string),
        AsPropertyExpression(string_predicate->left.get()));
  } else if (expression->Is(ast::ASTNodeType::kNullPredicateExpression)) {
    const auto *null_predicate =
        ast::CastAst<ast::NullPredicateExpression>(expression);
    auto property_null = std::make_unique<PropertyNullPredicate>();
    property_null->is_null = null_predicate->is_null;
    predicate = MakePropertyPredicate(
        std::move(property_null),
        AsPropertyExpression(null_predicate->operand.get()));
  } else if (AsExistentialSubquery(expression) != nullptr ||
             AsPatternPredicateExpression(expression) != nullptr) {
    predicate = std::make_unique<ExistsPredicate>();
  } else if (expression->Is(ast::ASTNodeType::kNotExpression)) {
    const auto *not_expression = ast::CastAst<ast::NotExpression>(expression);
    const ast::Expression *operand = not_expression->operand.get();
    if (AsExistentialSubquery(operand) != nullptr ||
        AsPatternPredicateExpression(operand) != nullptr) {
      auto exists = std::make_unique<ExistsPredicate>();
      exists->negated = true;
      predicate = std::move(exists);
    }
  }
  if (predicate == nullptr) {
    predicate = std::make_unique<GenericPredicate>();
  }
  predicate->expression = expression;
  predicate->dependencies = semantic_table.ExpressionDependencies(*expression);
  return predicate;
}

void AddSelectionPredicates(const ast::Expression *where,
                            const ast::SemanticTable &semantic_table,
                            const QueryGraph *query_graph,
                            Selections *selections) {
  RG_CHECK(where != nullptr, common::ErrorCode::InvalidParameter,
           "selection predicate is null");
  RG_CHECK(selections != nullptr, common::ErrorCode::InternalError,
           "selections is null");
  std::vector<const ast::Expression *> predicates;
  SplitConjunctivePredicates(where, &predicates);
  RG_CHECK(!predicates.empty(), common::ErrorCode::InvalidParameter,
           "selection predicate list is empty");
  for (const ast::Expression *predicate : predicates) {
    RG_CHECK(predicate != nullptr, common::ErrorCode::InvalidParameter,
             "null selection predicate is not supported");

    selections->AddPredicate(
        BuildPredicate(predicate, semantic_table, query_graph));
  }
}

}  // namespace ir
