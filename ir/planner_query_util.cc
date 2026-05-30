#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_const_walker.h"
#include "ast/expression_dependency.h"
#include "ast/expression_to_string.h"
#include "ast/semantic_table.h"
#include "common/exception.h"
#include "ir/planner_query_internal.h"

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

const ast::PatternPredicateExpression *AsPatternPredicateExpression(
    const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr ||
      !unwrapped->Is(ast::ASTNodeType::kPatternPredicateExpression)) {
    return nullptr;
  }
  return ast::CastAst<ast::PatternPredicateExpression>(unwrapped);
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
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
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
  CHECK(symbols != nullptr, common::InternalError, "symbol set is null");
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
  THROW(common::InternalError, "unknown mutating pattern kind");
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

void AddPropertyMapDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const PatternPropertyMap &properties) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  for (const auto &entry : properties.entries) {
    AddExpressionDependencySymbols(dependencies, entry.value);
  }
  AddExpressionDependencySymbols(dependencies, properties.parameter);
}

void AddCreatePatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const CreatePattern &pattern) {
  CHECK(dependencies != nullptr, common::InternalError,
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
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  AddExpressionDependencySymbols(dependencies, pattern.entity);
  AddExpressionDependencySymbols(dependencies, pattern.value);
}

void AddSetMutatingPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const std::vector<SetMutatingPattern> &patterns) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  for (const auto &pattern : patterns) {
    AddSetMutatingPatternDependencySymbols(dependencies, pattern);
  }
}

void AddRemoveMutatingPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const RemoveMutatingPattern &pattern) {
  CHECK(dependencies != nullptr, common::InternalError,
        "dependency set is null");
  AddExpressionDependencySymbols(dependencies, pattern.entity);
}

void AddRemoveMutatingPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const std::vector<RemoveMutatingPattern> &patterns) {
  CHECK(dependencies != nullptr, common::InternalError,
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

bool IsPropertyPredicateKind(PredicateKind kind) {
  return kind == PredicateKind::kPropertyEquality ||
         kind == PredicateKind::kPropertyComparison ||
         kind == PredicateKind::kPropertyIn ||
         kind == PredicateKind::kPropertyStringPredicate ||
         kind == PredicateKind::kPropertyIsNull ||
         kind == PredicateKind::kPropertyIsNotNull;
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
      CHECK(predicate.property_value != nullptr, common::InvalidArgumentError,
            "property predicate value is null");
      AppendKeyPart(&key, ExpressionKey(*predicate.property_value));
      break;
    case PredicateKind::kPropertyIn:
    case PredicateKind::kPropertyStringPredicate:
      AppendKeyPart(&key, predicate.variable);
      AppendKeyPart(&key, predicate.property_key);
      AppendKeyPart(&key, predicate.comparison_op);
      CHECK(predicate.property_value != nullptr, common::InvalidArgumentError,
            "property predicate value is null");
      AppendKeyPart(&key, ExpressionKey(*predicate.property_value));
      break;
    case PredicateKind::kPropertyIsNull:
    case PredicateKind::kPropertyIsNotNull:
      AppendKeyPart(&key, predicate.variable);
      AppendKeyPart(&key, predicate.property_key);
      break;
    case PredicateKind::kGenericExpression:
    case PredicateKind::kExistsSubquery:
    case PredicateKind::kNotExistsSubquery:
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

bool FillPropertyPredicate(Predicate *predicate,
                           const ast::PropertyExpression *property,
                           const ast::Expression *value,
                           std::string comparison_op, PredicateKind kind) {
  CHECK(predicate != nullptr, common::InternalError, "predicate is null");
  if (property == nullptr) {
    return false;
  }
  const ast::Variable *variable = AsVariableExpression(property->object.get());
  if (variable == nullptr || property->property_key.empty()) {
    return false;
  }
  predicate->variable = variable->name;
  predicate->property_key = property->property_key;
  predicate->property_value = value;
  predicate->comparison_op = std::move(comparison_op);
  predicate->kind = kind;
  return true;
}

void AddAssertIsNodeVariables(QueryGraph *query_graph) {
  CHECK(query_graph != nullptr, common::InternalError, "query graph is null");
  for (const auto &node : query_graph->pattern_nodes) {
    if (query_graph->argument_ids.contains(node) &&
        !QueryGraphRelationshipCoversVariable(*query_graph, node)) {
      query_graph->assert_is_node_variables.insert(node);
    }
  }
}

void AddNestedPredicateInfo(Predicate *predicate,
                            const ast::SemanticTable &semantic_table,
                            const QueryGraph *query_graph) {
  CHECK(predicate != nullptr, common::InternalError, "predicate is null");
  CHECK(predicate->expression != nullptr, common::InvalidArgumentError,
        "predicate expression is null");

  class Collector final : public ast::ASTConstWalker {
   public:
    Collector(Predicate *predicate, const ast::SemanticTable &semantic_table,
              const QueryGraph *query_graph)
        : predicate_(predicate),
          semantic_table_(semantic_table),
          query_graph_(query_graph) {}

   protected:
    void Visit(const ast::LabelPredicateExpression &node) override {
      const ast::Variable *variable = AsVariableExpression(node.expr.get());
      if (variable != nullptr) {
        auto variable_type =
            semantic_table_.VariableTypeAt(node, variable->name);
        if (!variable_type.has_value()) {
          variable_type = semantic_table_.VariableType(variable->name);
        }
        const bool type_unknown =
            !variable_type.has_value() ||
            *variable_type == ast::SemanticVariableType::kUnknown;
        if (variable_type == ast::SemanticVariableType::kRelationship ||
            (type_unknown && QueryGraphContainsRelationshipVariable(
                                 query_graph_, variable->name))) {
          predicate_->nested_relationship_types.push_back(
              {.variable = variable->name, .relationship_types = node.labels});
        } else {
          predicate_->nested_node_labels.push_back(
              {.variable = variable->name, .labels = node.labels});
        }
      }
      ast::ASTConstWalker::Visit(node);
    }

    void Visit(const ast::PropertyExpression &node) override {
      const ast::Variable *variable = AsVariableExpression(node.object.get());
      if (variable != nullptr && !node.property_key.empty()) {
        predicate_->nested_properties.push_back(
            {.variable = variable->name, .property_key = node.property_key});
      }
      ast::ASTConstWalker::Visit(node);
    }

   private:
    Predicate *predicate_ = nullptr;
    const ast::SemanticTable &semantic_table_;
    const QueryGraph *query_graph_ = nullptr;
  };

  Collector collector(predicate, semantic_table, query_graph);
  predicate->expression->Accept(collector);
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
    const ast::Expression *value = comparison->right.get();
    std::string comparison_op = comparison->op;
    if (property == nullptr) {
      property = AsPropertyExpression(comparison->right.get());
      value = comparison->left.get();
      comparison_op = ReverseComparisonOp(comparison->op);
    }
    const PredicateKind kind = comparison_op == "="
                                   ? PredicateKind::kPropertyEquality
                                   : PredicateKind::kPropertyComparison;
    FillPropertyPredicate(predicate, property, value, std::move(comparison_op),
                          kind);
    return;
  }

  if (expression->Is(ast::ASTNodeType::kListPredicateExpression)) {
    const auto *list_predicate =
        ast::CastAst<ast::ListPredicateExpression>(expression);
    FillPropertyPredicate(
        predicate, AsPropertyExpression(list_predicate->element.get()),
        list_predicate->list.get(), "IN", PredicateKind::kPropertyIn);
    return;
  }

  if (expression->Is(ast::ASTNodeType::kStringPredicateExpression)) {
    const auto *string_predicate =
        ast::CastAst<ast::StringPredicateExpression>(expression);
    FillPropertyPredicate(predicate,
                          AsPropertyExpression(string_predicate->left.get()),
                          string_predicate->right.get(), string_predicate->op,
                          PredicateKind::kPropertyStringPredicate);
    return;
  }

  if (expression->Is(ast::ASTNodeType::kNullPredicateExpression)) {
    const auto *null_predicate =
        ast::CastAst<ast::NullPredicateExpression>(expression);
    FillPropertyPredicate(
        predicate, AsPropertyExpression(null_predicate->operand.get()), nullptr,
        null_predicate->is_null ? "IS NULL" : "IS NOT NULL",
        null_predicate->is_null ? PredicateKind::kPropertyIsNull
                                : PredicateKind::kPropertyIsNotNull);
    return;
  }

  if (AsExistentialSubquery(expression) != nullptr ||
      AsPatternPredicateExpression(expression) != nullptr) {
    predicate->kind = PredicateKind::kExistsSubquery;
    return;
  }

  if (expression->Is(ast::ASTNodeType::kNotExpression)) {
    const auto *not_expression = ast::CastAst<ast::NotExpression>(expression);
    const ast::Expression *operand =
        UnwrapParenthesized(not_expression->operand.get());
    if (AsExistentialSubquery(operand) != nullptr ||
        AsPatternPredicateExpression(operand) != nullptr) {
      predicate->kind = PredicateKind::kNotExistsSubquery;
    }
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
    AddNestedPredicateInfo(&selection_predicate, semantic_table, query_graph);
    const std::string predicate_key = PredicateKey(selection_predicate);
    if (!selection_keys->insert(predicate_key).second ||
        !selections->AddPredicate(std::move(selection_predicate))) {
      continue;
    }
  }
}

}  // namespace ir
