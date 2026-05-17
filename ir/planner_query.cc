#include "ir/planner_query.h"

#include <algorithm>
#include <iterator>
#include <memory>
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
         kind == PredicateKind::kPropertyComparison;
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

  if (AsExistentialSubquery(expression) != nullptr ||
      AsPatternPredicateExpression(expression) != nullptr) {
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
    AddNestedPredicateInfo(&selection_predicate, semantic_table, query_graph);
    const std::string predicate_key = PredicateKey(selection_predicate);
    if (!selection_keys->insert(predicate_key).second ||
        !selections->AddPredicate(std::move(selection_predicate))) {
      continue;
    }
  }
}

}  // namespace

bool Selections::AddPredicate(Predicate predicate) {
  const std::string key = PredicateKey(predicate);
  if (!predicate_keys.insert(key).second) {
    return false;
  }
  predicates.push_back(std::move(predicate));
  return true;
}

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

std::vector<const Predicate *> Selections::PredicatesGiven(
    const std::unordered_set<std::string> &bound_symbols) const {
  std::vector<const Predicate *> result;
  for (const auto &predicate : predicates) {
    if (DependenciesMet(predicate.dependencies, bound_symbols)) {
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

std::vector<PropertyInequalityGroup> Selections::PropertyInequalityGroups()
    const {
  std::vector<PropertyInequalityGroup> groups;
  for (const auto &predicate : predicates) {
    if (predicate.kind != PredicateKind::kPropertyComparison ||
        predicate.variable.empty() || predicate.property_key.empty()) {
      continue;
    }
    if (!IsLowerBoundComparison(predicate.comparison_op) &&
        !IsUpperBoundComparison(predicate.comparison_op)) {
      continue;
    }
    auto found = std::find_if(groups.begin(), groups.end(),
                              [&](const PropertyInequalityGroup &g) {
                                return g.variable == predicate.variable &&
                                       g.property_key == predicate.property_key;
                              });
    if (found == groups.end()) {
      PropertyInequalityGroup group;
      group.variable = predicate.variable;
      group.property_key = predicate.property_key;
      groups.push_back(std::move(group));
      found = std::prev(groups.end());
    }
    if (IsLowerBoundComparison(predicate.comparison_op)) {
      found->lower_bounds.push_back(&predicate);
    } else {
      found->upper_bounds.push_back(&predicate);
    }
  }
  return groups;
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

  void AddRelationshipsPattern(const ast::RelationshipsPattern &pattern) {
    CHECK(pattern.node_pattern != nullptr, common::InvalidArgumentError,
          Missing("relationships pattern node"));
    CHECK(!pattern.chain.empty(), common::InvalidArgumentError,
          Missing("relationships pattern chain"));
    std::string left = AddNode(*pattern.node_pattern);
    for (const auto &link : pattern.chain) {
      CHECK(link.first != nullptr, common::InvalidArgumentError,
            Missing("relationship pattern"));
      CHECK(link.second != nullptr, common::InvalidArgumentError,
            Missing("node pattern"));
      std::string right = AddNode(*link.second);
      AddRelationship(*link.first, left, right);
      left = right;
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

Direction PatternDirection(const ast::RelationshipPattern &pattern) {
  if (pattern.left_arrow) {
    return Direction::kIncoming;
  }
  if (pattern.right_arrow) {
    return Direction::kOutgoing;
  }
  return Direction::kBoth;
}

class CreatePatternConverter {
 public:
  explicit CreatePatternConverter(
      std::unordered_set<LogicalVariable> existing_nodes = {})
      : existing_nodes_(std::move(existing_nodes)) {}

  CreatePattern Convert(const ast::Pattern &pattern) {
    CHECK(!pattern.parts.empty(), common::InvalidArgumentError,
          Missing("updating pattern part"));
    for (const auto &part : pattern.parts) {
      CHECK(part != nullptr, common::InvalidArgumentError,
            Missing("updating pattern part"));
      AddPatternPart(*part);
    }
    return std::move(pattern_);
  }

  CreatePattern Convert(const ast::PatternPart &part) {
    AddPatternPart(part);
    return std::move(pattern_);
  }

 private:
  void AddPatternPart(const ast::PatternPart &part) {
    if (!part.variable.empty()) {
      pattern_.path_variables.insert(part.variable);
    }
    CHECK(part.element != nullptr, common::InvalidArgumentError,
          Missing("updating pattern element"));
    AddPatternElement(*part.element);
  }

  void AddPatternElement(const ast::PatternElement &element) {
    CHECK(element.node_pattern != nullptr, common::InvalidArgumentError,
          Missing("updating node pattern"));
    std::string left = AddNode(*element.node_pattern);
    for (const auto &link : element.chain) {
      CHECK(link.first != nullptr, common::InvalidArgumentError,
            Missing("updating relationship pattern"));
      CHECK(link.second != nullptr, common::InvalidArgumentError,
            Missing("updating node pattern"));
      std::string right = AddNode(*link.second);
      AddRelationship(*link.first, left, right);
      left = std::move(right);
    }
  }

  std::string AddNode(const ast::NodePattern &node) {
    CHECK(!node.variable.empty(), common::InvalidArgumentError,
          Unsupported("anonymous node in updating pattern"));
    if (node_indexes_.contains(node.variable)) {
      CHECK(node.labels.empty() && !node.properties,
            common::InvalidArgumentError,
            Unsupported("reused updating node with labels or properties"));
      return node.variable;
    }

    CreateNodePattern create_node;
    create_node.variable = node.variable;
    create_node.labels = node.labels;
    create_node.properties = BuildPropertyMap(node.properties.get());
    create_node.previously_bound = existing_nodes_.contains(node.variable);
    if (create_node.previously_bound) {
      CHECK(create_node.labels.empty() && create_node.properties.empty(),
            common::InvalidArgumentError,
            Unsupported("bound updating node with labels or properties"));
    }

    const std::size_t index = pattern_.nodes.size();
    pattern_.nodes.push_back(std::move(create_node));
    node_indexes_.emplace(node.variable, index);
    if (!pattern_.nodes.back().previously_bound) {
      pattern_.commands.push_back(
          {.kind = CreateEntityKind::kNode, .index = index});
    }
    return node.variable;
  }

  void AddRelationship(const ast::RelationshipPattern &pattern,
                       const std::string &left, const std::string &right) {
    const ast::RelationshipDetail *detail = pattern.detail.get();
    CHECK(detail != nullptr && !detail->variable.empty(),
          common::InvalidArgumentError,
          Unsupported("anonymous relationship in updating pattern"));
    CHECK(!detail->range.has_value(), common::InvalidArgumentError,
          Unsupported("variable-length relationship in updating pattern"));

    CreateRelationshipPattern create_relationship;
    create_relationship.variable = detail->variable;
    create_relationship.left_node = left;
    create_relationship.right_node = right;
    create_relationship.direction = PatternDirection(pattern);
    create_relationship.types = detail->types;
    create_relationship.properties = BuildPropertyMap(detail->properties.get());

    const std::size_t index = pattern_.relationships.size();
    pattern_.relationships.push_back(std::move(create_relationship));
    pattern_.commands.push_back(
        {.kind = CreateEntityKind::kRelationship, .index = index});
  }

  CreatePattern pattern_;
  std::unordered_map<std::string, std::size_t> node_indexes_;
  std::unordered_set<LogicalVariable> existing_nodes_;
};

CreatePattern BuildCreatePattern(
    const ast::Pattern &pattern,
    std::unordered_set<LogicalVariable> existing_nodes = {}) {
  CreatePatternConverter converter(std::move(existing_nodes));
  return converter.Convert(pattern);
}

CreatePattern BuildCreatePattern(
    const ast::PatternPart &part,
    std::unordered_set<LogicalVariable> existing_nodes = {}) {
  CreatePatternConverter converter(std::move(existing_nodes));
  return converter.Convert(part);
}

PatternRelationship ToPatternRelationship(
    const CreateRelationshipPattern &relationship) {
  PatternRelationship out;
  out.variable = relationship.variable;
  out.left_node = relationship.left_node;
  out.right_node = relationship.right_node;
  out.direction = relationship.direction;
  out.types = relationship.types;
  return out;
}

MergeMatchGraph BuildMergeMatchGraph(
    const CreatePattern &pattern,
    std::unordered_set<LogicalVariable> argument_ids) {
  MergeMatchGraph match_graph;
  match_graph.argument_ids = std::move(argument_ids);
  for (const auto &node : pattern.nodes) {
    AddSymbol(&match_graph.pattern_nodes, node.variable);
    if (!node.labels.empty()) {
      match_graph.node_labels.push_back(
          {.variable = node.variable, .labels = node.labels});
    }
    for (const auto &entry : node.properties.entries) {
      match_graph.property_equalities.push_back({.variable = node.variable,
                                                 .property_key = entry.key,
                                                 .value = entry.value});
    }
  }
  for (const auto &relationship : pattern.relationships) {
    AddSymbol(&match_graph.pattern_nodes, relationship.left_node);
    AddSymbol(&match_graph.pattern_nodes, relationship.right_node);
    match_graph.pattern_relationships.push_back(
        ToPatternRelationship(relationship));
    for (const auto &entry : relationship.properties.entries) {
      match_graph.property_equalities.push_back(
          {.variable = relationship.variable,
           .property_key = entry.key,
           .value = entry.value});
    }
  }
  return match_graph;
}

SetMutatingPattern BuildSetMutatingPattern(const ast::SetItem &item) {
  SetMutatingPattern pattern;
  switch (item.type) {
    case ast::SetItem::Type::kProperty: {
      const ast::PropertyExpression *property =
          AsPropertyExpression(item.target.get());
      CHECK(property != nullptr, common::InvalidArgumentError,
            Missing("SET property target"));
      pattern.kind = SetMutatingPatternKind::kSetProperty;
      pattern.entity = property->object.get();
      pattern.property_key = property->property_key;
      pattern.value = item.value.get();
      return pattern;
    }
    case ast::SetItem::Type::kVariable:
      pattern.kind =
          item.plus_equal
              ? SetMutatingPatternKind::kSetIncludingPropertiesFromMap
              : SetMutatingPatternKind::kSetExactPropertiesFromMap;
      pattern.entity = item.target.get();
      pattern.value = item.value.get();
      return pattern;
    case ast::SetItem::Type::kLabels:
      pattern.kind = SetMutatingPatternKind::kSetLabels;
      pattern.entity = item.target.get();
      pattern.labels = item.labels;
      return pattern;
  }
  THROW(common::InternalError, "unknown SET item kind");
}

std::vector<SetMutatingPattern> BuildSetMutatingPatterns(const ast::Set &set) {
  std::vector<SetMutatingPattern> patterns;
  patterns.reserve(set.items.size());
  for (const auto &item : set.items) {
    CHECK(item != nullptr, common::InvalidArgumentError, Missing("SET item"));
    patterns.push_back(BuildSetMutatingPattern(*item));
  }
  return patterns;
}

RemoveMutatingPattern BuildRemoveMutatingPattern(const ast::RemoveItem &item) {
  RemoveMutatingPattern pattern;
  switch (item.type) {
    case ast::RemoveItem::Type::kProperty: {
      const ast::PropertyExpression *property =
          AsPropertyExpression(item.target.get());
      CHECK(property != nullptr, common::InvalidArgumentError,
            Missing("REMOVE property target"));
      pattern.kind = RemoveMutatingPatternKind::kRemoveProperty;
      pattern.entity = property->object.get();
      pattern.property_key = property->property_key;
      return pattern;
    }
    case ast::RemoveItem::Type::kLabels:
      pattern.kind = RemoveMutatingPatternKind::kRemoveLabels;
      pattern.entity = item.target.get();
      pattern.labels = item.labels;
      return pattern;
  }
  THROW(common::InternalError, "unknown REMOVE item kind");
}

std::vector<RemoveMutatingPattern> BuildRemoveMutatingPatterns(
    const ast::Remove &remove) {
  std::vector<RemoveMutatingPattern> patterns;
  patterns.reserve(remove.items.size());
  for (const auto &item : remove.items) {
    CHECK(item != nullptr, common::InvalidArgumentError,
          Missing("REMOVE item"));
    patterns.push_back(BuildRemoveMutatingPattern(*item));
  }
  return patterns;
}

MergePattern BuildMergePattern(
    const ast::Merge &merge,
    const std::unordered_set<std::string> &available_arguments,
    std::unordered_set<LogicalVariable> existing_nodes) {
  CHECK(merge.pattern_part != nullptr, common::InvalidArgumentError,
        Missing("MERGE pattern"));
  MergePattern pattern;
  pattern.create_pattern =
      BuildCreatePattern(*merge.pattern_part, std::move(existing_nodes));
  pattern.actions.reserve(merge.actions.size());
  for (const auto &action : merge.actions) {
    CHECK(action.second != nullptr, common::InvalidArgumentError,
          Missing("MERGE action SET"));
    MergeActionPattern action_pattern;
    action_pattern.on_match = action.first;
    action_pattern.set_patterns = BuildSetMutatingPatterns(*action.second);
    pattern.actions.push_back(std::move(action_pattern));
  }

  std::unordered_set<std::string> dependencies;
  AddCreatePatternDependencySymbols(&dependencies, pattern.create_pattern);
  for (const auto &action : pattern.actions) {
    AddSetMutatingPatternDependencySymbols(&dependencies, action.set_patterns);
  }
  pattern.match_graph =
      BuildMergeMatchGraph(pattern.create_pattern,
                           IntersectSymbols(dependencies, available_arguments));
  return pattern;
}

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
  [[nodiscard]] bool HasOptionalMatches() const {
    return !graph_.optional_matches.empty();
  }
  [[nodiscard]] bool ContainsUpdates() const {
    return graph_.ContainsUpdates();
  }
  [[nodiscard]] std::unordered_set<LogicalVariable> AvailableSymbols() const {
    return QueryGraphAvailableSymbols(graph_);
  }

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
  [[nodiscard]] std::unordered_set<LogicalVariable> CurrentNodeSymbols() const {
    std::unordered_set<LogicalVariable> symbols = graph_.argument_ids;
    AddSymbols(&symbols, graph_.pattern_nodes);
    for (const auto &mutating_pattern : graph_.mutating_patterns) {
      switch (mutating_pattern.kind) {
        case MutatingPatternKind::kCreate:
          AddCreatePatternNodeSymbols(&symbols, mutating_pattern.create);
          break;
        case MutatingPatternKind::kMerge:
          AddSymbols(&symbols,
                     mutating_pattern.merge.match_graph.pattern_nodes);
          break;
        case MutatingPatternKind::kSet:
        case MutatingPatternKind::kDelete:
        case MutatingPatternKind::kRemove:
          break;
      }
    }
    return symbols;
  }

  MutatingPattern BuildMutatingPattern(
      const ast::UpdatingClause &clause) const {
    MutatingPattern mutating_pattern;
    mutating_pattern.clause = &clause;
    switch (clause.node_type) {
      case ast::ASTNodeType::kCreate: {
        const auto &create = ast::CastAst<ast::Create>(clause);
        CHECK(create.pattern != nullptr, common::InvalidArgumentError,
              Missing("CREATE pattern"));
        mutating_pattern.kind = MutatingPatternKind::kCreate;
        mutating_pattern.create =
            BuildCreatePattern(*create.pattern, CurrentNodeSymbols());
        return mutating_pattern;
      }
      case ast::ASTNodeType::kMerge: {
        const auto &merge = ast::CastAst<ast::Merge>(clause);
        mutating_pattern.kind = MutatingPatternKind::kMerge;
        mutating_pattern.merge =
            BuildMergePattern(merge, graph_.argument_ids, CurrentNodeSymbols());
        return mutating_pattern;
      }
      case ast::ASTNodeType::kSet: {
        const auto &set = ast::CastAst<ast::Set>(clause);
        mutating_pattern.kind = MutatingPatternKind::kSet;
        mutating_pattern.set_patterns = BuildSetMutatingPatterns(set);
        return mutating_pattern;
      }
      case ast::ASTNodeType::kDelete: {
        const auto &del = ast::CastAst<ast::Delete>(clause);
        mutating_pattern.kind = MutatingPatternKind::kDelete;
        mutating_pattern.delete_patterns.reserve(del.expressions.size());
        for (const auto &expression : del.expressions) {
          CHECK(expression != nullptr, common::InvalidArgumentError,
                Missing("DELETE expression"));
          mutating_pattern.delete_patterns.push_back(
              {.expression = expression.get(), .detach = del.detach});
        }
        return mutating_pattern;
      }
      case ast::ASTNodeType::kRemove: {
        const auto &remove = ast::CastAst<ast::Remove>(clause);
        mutating_pattern.kind = MutatingPatternKind::kRemove;
        mutating_pattern.remove_patterns = BuildRemoveMutatingPatterns(remove);
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

QueryHorizon QueryHorizon::ForProcedureCall(
    ProcedureCallHorizon procedure_call) {
  QueryHorizon horizon;
  horizon.kind = QueryHorizonKind::kProcedureCall;
  horizon.procedure_call = std::move(procedure_call);
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

const ProcedureCallHorizon &QueryHorizon::RequireProcedureCall() const {
  CHECK(kind == QueryHorizonKind::kProcedureCall, common::InvalidArgumentError,
        Unsupported("procedure call horizon"));
  return procedure_call;
}

ProcedureCallHorizon &QueryHorizon::RequireProcedureCall() {
  CHECK(kind == QueryHorizonKind::kProcedureCall, common::InvalidArgumentError,
        Unsupported("procedure call horizon"));
  return procedure_call;
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
  query->distinct = !all;
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
            BuildRegularQuery(ast::CastAst<ast::RegularQuery>(statement),
                              ProjectionPosition::kFinal);
        FinalizePlannerQueryArguments(*planner_query);
        return planner_query;
      }
      case ast::ASTNodeType::kStandaloneCall: {
        std::unique_ptr<PlannerQuery> planner_query =
            BuildStandaloneCall(ast::CastAst<ast::StandaloneCall>(statement));
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
      const ast::RegularQuery &query, ProjectionPosition projection_position) {
    CHECK(query.single_query, common::InvalidArgumentError,
          Missing("single query"));

    const ProjectionPosition branch_position =
        query.unions.empty() ? projection_position
                             : ProjectionPosition::kIntermediate;
    std::unique_ptr<PlannerQuery> planner_query = MakeSinglePlannerQuery(
        BuildSingleQuery(*query.single_query, branch_position));
    for (const auto &part : query.unions) {
      CHECK(part && part->query, common::InvalidArgumentError,
            Missing("UNION branch query"));
      SinglePlannerQuery rhs =
          BuildSingleQuery(*part->query, ProjectionPosition::kIntermediate);
      const std::vector<std::string> lhs_columns =
          PlannerQueryOutputAliases(*planner_query);
      const std::vector<std::string> rhs_columns =
          SinglePlannerQueryOutputAliases(rhs);
      std::unique_ptr<PlannerQuery> union_query = MakeUnionPlannerQuery(
          std::move(planner_query), std::move(rhs), part->all);
      union_query->RequireUnion().mappings =
          BuildUnionMappings(lhs_columns, rhs_columns);
      planner_query = std::move(union_query);
    }

    return planner_query;
  }

  std::unique_ptr<PlannerQuery> BuildStandaloneCall(
      const ast::StandaloneCall &call) {
    SinglePlannerQuery query;
    query.horizon = QueryHorizon::ForProcedureCall(
        BuildProcedureCallHorizon(call, /*expand_implicit_yields=*/true));
    AttachQueryHorizonSubqueries(&query.horizon);
    return MakeSinglePlannerQuery(std::move(query));
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
        AttachNestedIRExpressions(
            &horizon->RequireRegularProjection().nested_expressions);
        return;
      case QueryHorizonKind::kDistinctProjection:
        AttachSelectionSubqueries(
            &horizon->RequireDistinctProjection().selections);
        AttachNestedIRExpressions(
            &horizon->RequireDistinctProjection().nested_expressions);
        return;
      case QueryHorizonKind::kAggregatingProjection:
        AttachSelectionSubqueries(
            &horizon->RequireAggregatingProjection().selections);
        AttachNestedIRExpressions(
            &horizon->RequireAggregatingProjection().nested_expressions);
        return;
      case QueryHorizonKind::kProcedureCall:
        AttachSelectionSubqueries(
            &horizon->RequireProcedureCall().yield_selections);
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
      AppendNestedIRExpressions(
          &predicate.nested_expressions,
          CollectNestedIRExpressions(predicate.expression));
      for (auto &nested : predicate.nested_expressions) {
        if (nested.kind == NestedIRExpressionKind::kExists &&
            nested.query != nullptr) {
          predicate.subquery = nested.query.get();
          break;
        }
      }
      if (predicate.kind != PredicateKind::kExistsSubquery) {
        continue;
      }
      CHECK(predicate.subquery != nullptr, common::InvalidArgumentError,
            Missing("EXISTS nested IR expression"));
    }
  }

  void AttachNestedIRExpressions(
      std::vector<NestedIRExpression> *nested_expressions) {
    CHECK(nested_expressions != nullptr, common::InternalError,
          "nested expression list is null");
    for (auto &nested : *nested_expressions) {
      CHECK(nested.query != nullptr, common::InvalidArgumentError,
            "nested IR expression query is null");
    }
  }

  static void AppendNestedIRExpressions(
      std::vector<NestedIRExpression> *target,
      std::vector<NestedIRExpression> incoming) {
    CHECK(target != nullptr, common::InternalError,
          "nested expression target is null");
    target->reserve(target->size() + incoming.size());
    for (auto &nested : incoming) {
      target->push_back(std::move(nested));
    }
  }

  std::string NextNestedVariable(std::string_view prefix) {
    std::string out = "__";
    out.append(prefix.data(), prefix.size());
    out.push_back('_');
    out.append(std::to_string(nested_expression_id_++));
    return out;
  }

  std::vector<NestedIRExpression> CollectNestedIRExpressions(
      const ast::Expression *expression) {
    std::vector<NestedIRExpression> nested_expressions;
    if (expression == nullptr) {
      return nested_expressions;
    }

    class Collector final : public ast::ASTConstWalker {
     public:
      Collector(PlannerQueryBuilder *builder,
                std::vector<NestedIRExpression> *nested_expressions)
          : builder_(builder), nested_expressions_(nested_expressions) {}

     protected:
      void Visit(const ast::ExistentialSubquery &node) override {
        nested_expressions_->push_back(builder_->BuildExistsIRExpression(node));
      }

      void Visit(const ast::PatternComprehension &node) override {
        nested_expressions_->push_back(builder_->BuildListIRExpression(node));
        WalkMaybe(node.where_expr);
        WalkMaybe(node.eval_expr);
      }

     private:
      PlannerQueryBuilder *builder_ = nullptr;
      std::vector<NestedIRExpression> *nested_expressions_ = nullptr;
    };

    Collector collector(this, &nested_expressions);
    expression->Accept(collector);
    return nested_expressions;
  }

  NestedIRExpression BuildExistsIRExpression(
      const ast::ExistentialSubquery &exists) {
    NestedIRExpression nested;
    nested.kind = NestedIRExpressionKind::kExists;
    nested.expression = &exists;
    nested.dependencies = SemanticTableRef().ExpressionDependencies(exists);
    nested.value_variable = NextNestedVariable("exists");
    if (exists.query != nullptr) {
      nested.query =
          BuildRegularQuery(*exists.query, ProjectionPosition::kIntermediate);
    } else {
      nested.query = BuildPatternExistsQuery(exists);
    }
    return nested;
  }

  std::unique_ptr<PlannerQuery> BuildPatternExistsQuery(
      const ast::ExistentialSubquery &exists) {
    CHECK(exists.pattern != nullptr, common::InvalidArgumentError,
          Missing("EXISTS pattern"));
    SinglePlannerQuery query;
    PatternConverter converter(&query.query_graph);
    converter.AddPattern(*exists.pattern);
    if (exists.where_expr != nullptr) {
      std::unordered_set<std::string> selection_keys;
      AddSelectionPredicates(exists.where_expr.get(), SemanticTableRef(),
                             &query.query_graph, &query.query_graph.selections,
                             &selection_keys);
    }
    AttachQueryGraphSubqueries(&query.query_graph);
    query.horizon = QueryHorizon::ForPassthrough();
    return MakeSinglePlannerQuery(std::move(query));
  }

  NestedIRExpression BuildListIRExpression(
      const ast::PatternComprehension &comprehension) {
    CHECK(comprehension.relationships_pattern != nullptr,
          common::InvalidArgumentError,
          Missing("pattern comprehension relationships pattern"));
    CHECK(comprehension.eval_expr != nullptr, common::InvalidArgumentError,
          Missing("pattern comprehension eval expression"));

    NestedIRExpression nested;
    nested.kind = NestedIRExpressionKind::kList;
    nested.expression = &comprehension;
    nested.dependencies =
        SemanticTableRef().ExpressionDependencies(comprehension);
    nested.value_variable = NextNestedVariable("list_value");
    nested.collection_variable = comprehension.variable.empty()
                                     ? NextNestedVariable("list")
                                     : comprehension.variable;

    SinglePlannerQuery query;
    PatternConverter converter(&query.query_graph);
    converter.AddRelationshipsPattern(*comprehension.relationships_pattern);
    if (comprehension.where_expr != nullptr) {
      std::unordered_set<std::string> selection_keys;
      AddSelectionPredicates(comprehension.where_expr.get(), SemanticTableRef(),
                             &query.query_graph, &query.query_graph.selections,
                             &selection_keys);
    }
    AttachQueryGraphSubqueries(&query.query_graph);

    RegularQueryProjection projection;
    projection.position = ProjectionPosition::kIntermediate;
    projection.items.push_back({.expression = comprehension.eval_expr.get(),
                                .alias = nested.value_variable});
    AppendNestedIRExpressions(
        &projection.nested_expressions,
        CollectNestedIRExpressions(comprehension.eval_expr.get()));
    query.horizon = QueryHorizon::ForRegularProjection(std::move(projection));
    AttachQueryHorizonSubqueries(&query.horizon);
    nested.query = MakeSinglePlannerQuery(std::move(query));
    return nested;
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
      std::unordered_set<std::string> horizon_available_symbols =
          QueryGraphAvailableSymbols(segment->query_graph);
      AddSymbols(&horizon_available_symbols, output_symbols);
      FinalizeQueryHorizonArguments(&segment->horizon,
                                    horizon_available_symbols);
      available_symbols = std::move(output_symbols);
      segment = segment->tail.get();
    }
  }

  void FinalizeQueryGraphArguments(QueryGraph *query_graph) const {
    CHECK(query_graph != nullptr, common::InternalError, "query graph is null");
    std::unordered_set<std::string> available_symbols =
        query_graph->IdsWithoutOptionalMatchesOrUpdates();
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
    AddAssertIsNodeVariables(query_graph);
  }

  void FinalizeSelectionSubqueries(
      Selections *selections,
      const std::unordered_set<std::string> &available_symbols) const {
    CHECK(selections != nullptr, common::InternalError, "selections is null");
    for (auto &predicate : selections->predicates) {
      for (auto &nested : predicate.nested_expressions) {
        FinalizeNestedIRExpression(
            &nested, IntersectSymbols(nested.dependencies, available_symbols));
      }
      if (predicate.subquery == nullptr &&
          predicate.kind == PredicateKind::kExistsSubquery) {
        THROW(common::InvalidArgumentError,
              Missing("EXISTS nested IR expression"));
      }
    }
  }

  void FinalizeNestedIRExpression(
      NestedIRExpression *nested,
      const std::unordered_set<std::string> &available_symbols) const {
    CHECK(nested != nullptr, common::InternalError,
          "nested IR expression is null");
    CHECK(nested->query != nullptr, common::InvalidArgumentError,
          "nested IR expression query is null");
    FinalizePlannerQueryArguments(*nested->query, available_symbols);
  }

  void FinalizeNestedIRExpressions(
      std::vector<NestedIRExpression> *nested_expressions,
      const std::unordered_set<std::string> &available_symbols) const {
    CHECK(nested_expressions != nullptr, common::InternalError,
          "nested IR expression list is null");
    for (auto &nested : *nested_expressions) {
      FinalizeNestedIRExpression(
          &nested, IntersectSymbols(nested.dependencies, available_symbols));
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
        FinalizeNestedIRExpressions(
            &horizon->RequireRegularProjection().nested_expressions,
            available_symbols);
        return;
      case QueryHorizonKind::kDistinctProjection:
        FinalizeSelectionSubqueries(
            &horizon->RequireDistinctProjection().selections,
            available_symbols);
        FinalizeNestedIRExpressions(
            &horizon->RequireDistinctProjection().nested_expressions,
            available_symbols);
        return;
      case QueryHorizonKind::kAggregatingProjection:
        FinalizeSelectionSubqueries(
            &horizon->RequireAggregatingProjection().selections,
            available_symbols);
        FinalizeNestedIRExpressions(
            &horizon->RequireAggregatingProjection().nested_expressions,
            available_symbols);
        return;
      case QueryHorizonKind::kProcedureCall:
        FinalizeSelectionSubqueries(
            &horizon->RequireProcedureCall().yield_selections,
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

  void AddProcedureCallDependencies(
      std::unordered_set<std::string> *dependencies,
      const ProcedureCallHorizon &procedure_call) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    for (const ast::Expression *argument : procedure_call.arguments) {
      AddExpressionDependencies(dependencies, argument);
    }
    AddSelectionDependencies(dependencies, procedure_call.yield_selections);
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

  void AddMutatingPatternDependencies(
      std::unordered_set<std::string> *dependencies,
      const MutatingPattern &mutating_pattern) const {
    CHECK(dependencies != nullptr, common::InternalError,
          "dependency set is null");
    AddSymbols(dependencies, MutatingPatternDependencies(mutating_pattern));
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
      case QueryHorizonKind::kProcedureCall:
        AddProcedureCallDependencies(&dependencies,
                                     horizon.RequireProcedureCall());
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
      case QueryHorizonKind::kProcedureCall: {
        std::unordered_set<std::string> symbols =
            QueryGraphAvailableSymbols(query.query_graph);
        for (const auto &item :
             query.horizon.RequireProcedureCall().yield_items) {
          AddSymbol(&symbols, item.variable);
        }
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

  static std::vector<std::string> ProjectionItemAliasList(
      const std::vector<ProjectionItem> &items) {
    std::vector<std::string> aliases;
    aliases.reserve(items.size());
    for (const auto &item : items) {
      if (!item.alias.empty()) {
        aliases.push_back(item.alias);
      }
    }
    return aliases;
  }

  static std::vector<std::string> SortedSymbolList(
      const std::unordered_set<std::string> &symbols) {
    std::vector<std::string> out(symbols.begin(), symbols.end());
    std::sort(out.begin(), out.end());
    return out;
  }

  static std::vector<std::string> SinglePlannerQueryOutputAliases(
      const SinglePlannerQuery &query) {
    const SinglePlannerQuery *last = query.Last();
    switch (last->horizon.kind) {
      case QueryHorizonKind::kRegularProjection:
        return ProjectionItemAliasList(
            last->horizon.RequireRegularProjection().items);
      case QueryHorizonKind::kDistinctProjection:
        return ProjectionItemAliasList(
            last->horizon.RequireDistinctProjection().grouping_items);
      case QueryHorizonKind::kAggregatingProjection: {
        std::vector<std::string> aliases = ProjectionItemAliasList(
            last->horizon.RequireAggregatingProjection().grouping_items);
        std::vector<std::string> aggregations = ProjectionItemAliasList(
            last->horizon.RequireAggregatingProjection().aggregation_items);
        aliases.insert(aliases.end(), aggregations.begin(), aggregations.end());
        return aliases;
      }
      case QueryHorizonKind::kProcedureCall: {
        std::vector<std::string> aliases;
        for (const auto &item :
             last->horizon.RequireProcedureCall().yield_items) {
          aliases.push_back(item.variable);
        }
        return aliases;
      }
      case QueryHorizonKind::kUnwind:
      case QueryHorizonKind::kPassthrough:
        return SortedSymbolList(QueryGraphAvailableSymbols(last->query_graph));
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  static std::vector<std::string> PlannerQueryOutputAliases(
      const PlannerQuery &query) {
    switch (query.Kind()) {
      case PlannerQueryKind::kSingle:
        return SinglePlannerQueryOutputAliases(query.RequireSingle());
      case PlannerQueryKind::kUnion: {
        const UnionPlannerQuery &union_query = query.RequireUnion();
        if (!union_query.mappings.empty()) {
          std::vector<std::string> aliases;
          aliases.reserve(union_query.mappings.size());
          for (const auto &mapping : union_query.mappings) {
            aliases.push_back(mapping.output_variable);
          }
          return aliases;
        }
        return SinglePlannerQueryOutputAliases(union_query.rhs);
      }
    }
    THROW(common::InternalError, "unknown planner query kind");
  }

  static std::vector<UnionPlannerQuery::UnionMapping> BuildUnionMappings(
      const std::vector<std::string> &lhs_columns,
      const std::vector<std::string> &rhs_columns) {
    CHECK(lhs_columns.size() == rhs_columns.size(),
          common::InvalidArgumentError,
          "UNION branches have different output column counts");
    std::vector<UnionPlannerQuery::UnionMapping> mappings;
    mappings.reserve(lhs_columns.size());
    for (std::size_t i = 0; i < lhs_columns.size(); ++i) {
      mappings.push_back({.output_variable = lhs_columns[i],
                          .lhs_variable = lhs_columns[i],
                          .rhs_variable = rhs_columns[i]});
    }
    return mappings;
  }

  bool IsPureVariablePassthrough(
      const ast::ProjectionBody &body,
      const std::unordered_set<std::string> &available_symbols) const {
    if (body.star || body.distinct || !body.order_by.empty() || body.skip ||
        body.limit) {
      return false;
    }

    std::unordered_set<std::string> projected_symbols;
    projected_symbols.reserve(body.items.size());
    for (const auto &item : body.items) {
      CHECK(item != nullptr, common::InvalidArgumentError,
            "projection item is null");
      CHECK(item->expression != nullptr, common::InvalidArgumentError,
            Missing("projection item expression"));
      if (SemanticTableRef().ContainsAggregation(*item->expression)) {
        return false;
      }
      const ast::Variable *variable =
          AsVariableExpression(item->expression.get());
      if (variable == nullptr || item->alias != variable->name ||
          !projected_symbols.insert(item->alias).second) {
        return false;
      }
    }
    return StringSetEquals(projected_symbols, available_symbols);
  }

  bool CanInlineWith(const ast::With &with_clause,
                     const std::unordered_set<std::string> &available_symbols,
                     bool has_optional_boundary,
                     bool has_update_boundary) const {
    if (has_optional_boundary || has_update_boundary) {
      return false;
    }
    CHECK(with_clause.body != nullptr, common::InvalidArgumentError,
          Missing("WITH body"));
    return IsPureVariablePassthrough(*with_clause.body, available_symbols);
  }

  SinglePlannerQuery BuildSingleQuery(const ast::SingleQuery &query,
                                      ProjectionPosition projection_position) {
    switch (query.node_type) {
      case ast::ASTNodeType::kSinglePartQuery: {
        return BuildSinglePartQuery(ast::CastAst<ast::SinglePartQuery>(query),
                                    projection_position);
      }
      case ast::ASTNodeType::kMultiPartQuery: {
        return BuildMultiPartQuery(ast::CastAst<ast::MultiPartQuery>(query),
                                   projection_position);
      }
      default: {
        THROW(common::InvalidArgumentError, Unsupported("single query type"));
      }
    }
  }

  SinglePlannerQuery BuildSinglePartQuery(
      const ast::SinglePartQuery &query,
      ProjectionPosition projection_position) {
    CHECK(query.return_clause || !query.updating_clauses.empty(),
          common::InvalidArgumentError, Missing("RETURN clause"));
    return BuildQuerySegment(query.reading_clauses, query.updating_clauses,
                             query.return_clause.get(), {},
                             projection_position);
  }

  SinglePlannerQuery BuildMultiPartQuery(
      const ast::MultiPartQuery &query,
      ProjectionPosition projection_position) {
    CHECK(query.final_single_part_query, common::InvalidArgumentError,
          Missing("final single query"));

    SinglePlannerQuery root;
    SinglePlannerQuery *current_segment = &root;
    std::unordered_set<std::string> current_argument_ids;
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

    auto build_reading_clauses =
        [&](const std::vector<std::unique_ptr<ast::ReadingClause>> &reading) {
          for (const auto &clause : reading) {
            CHECK(clause != nullptr, common::InvalidArgumentError,
                  Missing("reading clause"));
            if (clause->Is(ast::ASTNodeType::kUnwind)) {
              finish_and_start_next(QueryHorizon::ForUnwind(BuildUnwindHorizon(
                  *ast::CastAst<ast::Unwind>(clause.get()))));
              continue;
            }
            if (clause->Is(ast::ASTNodeType::kInQueryCall)) {
              finish_and_start_next(
                  QueryHorizon::ForProcedureCall(BuildProcedureCallHorizon(
                      *ast::CastAst<ast::InQueryCall>(clause.get()))));
              continue;
            }
            builder.BuildReadingClause(*clause);
          }
        };

    auto build_updating_clauses =
        [&](const std::vector<std::unique_ptr<ast::UpdatingClause>> &updating,
            bool has_following_work) {
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
              if (i + 1 < updating.size() || has_following_work) {
                start_next_segment();
              }
            }
          }
        };

    for (const auto &part : query.parts) {
      CHECK(part.with_clause, common::InvalidArgumentError,
            Missing("WITH clause"));
      CHECK(part.with_clause->body != nullptr, common::InvalidArgumentError,
            Missing("WITH body"));

      build_reading_clauses(part.reading_clauses);
      build_updating_clauses(part.updating_clauses,
                             /*has_following_work=*/true);

      const ast::With &with_clause = *part.with_clause;
      if (CanInlineWith(
              with_clause, builder.AvailableSymbols(),
              builder.HasOptionalMatches(),
              !part.updating_clauses.empty() || builder.ContainsUpdates())) {
        if (with_clause.where != nullptr) {
          builder.AddWhere(with_clause.where.get());
        }
        continue;
      }

      finish_and_start_next(BuildProjectionClause(
          &with_clause, ProjectionPosition::kIntermediate));
    }

    const ast::SinglePartQuery &final = *query.final_single_part_query;
    CHECK(final.return_clause || !final.updating_clauses.empty(),
          common::InvalidArgumentError, Missing("RETURN clause"));
    build_reading_clauses(final.reading_clauses);
    build_updating_clauses(final.updating_clauses,
                           final.return_clause != nullptr);
    if (final.return_clause != nullptr) {
      finish_current_segment(BuildProjectionClause(final.return_clause.get(),
                                                   projection_position));
    } else if (!current_segment_finished && builder.HasLocalWork()) {
      finish_current_segment(QueryHorizon::ForPassthrough());
    }
    return root;
  }

  SinglePlannerQuery BuildSinglePartQuery(
      const ast::SinglePartQuery &query,
      const std::unordered_set<std::string> &argument_ids) {
    CHECK(query.return_clause || !query.updating_clauses.empty(),
          common::InvalidArgumentError, Missing("RETURN clause"));
    return BuildQuerySegment(query.reading_clauses, query.updating_clauses,
                             query.return_clause.get(), argument_ids,
                             ProjectionPosition::kIntermediate);
  }

  struct ProjectionParts {
    bool distinct = false;
    ProjectionPosition position = ProjectionPosition::kIntermediate;
    std::vector<ProjectionItem> items;
    std::vector<ProjectionItem> grouping_items;
    std::vector<ProjectionItem> aggregation_items;
    RequiredOrder required_order;
    InterestingOrder interesting_order;
    const ast::Expression *skip = nullptr;
    const ast::Expression *limit = nullptr;
    std::vector<NestedIRExpression> nested_expressions;
  };

  static void MoveProjectionTail(ProjectionParts *parts,
                                 QueryProjection *projection) {
    CHECK(parts != nullptr, common::InternalError, "projection parts is null");
    CHECK(projection != nullptr, common::InternalError, "projection is null");
    projection->required_order = std::move(parts->required_order);
    projection->interesting_order = std::move(parts->interesting_order);
    projection->pagination.skip = parts->skip;
    projection->pagination.limit = parts->limit;
    projection->position = parts->position;
    projection->nested_expressions = std::move(parts->nested_expressions);
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
      case QueryHorizonKind::kProcedureCall:
        THROW(common::InternalError,
              "procedure call horizon cannot have projection WHERE");
      case QueryHorizonKind::kPassthrough:
        THROW(common::InternalError,
              "passthrough horizon cannot have projection WHERE");
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  QueryHorizon BuildProjectionClause(
      const ast::ProjectionClause *projection_clause,
      ProjectionPosition projection_position) {
    CHECK(projection_clause != nullptr, common::InvalidArgumentError,
          Missing("projection clause"));
    CHECK(projection_clause->body, common::InvalidArgumentError,
          Missing("projection body"));
    QueryHorizon horizon =
        BuildProjectionBody(*projection_clause->body, projection_position);
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
      const std::unordered_set<std::string> &argument_ids,
      ProjectionPosition projection_position) {
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
      if (clause->Is(ast::ASTNodeType::kInQueryCall)) {
        finish_and_start_next(
            QueryHorizon::ForProcedureCall(BuildProcedureCallHorizon(
                *ast::CastAst<ast::InQueryCall>(clause.get()))));
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
      finish_current_segment(
          BuildProjectionClause(projection, projection_position));
    } else if (!current_segment_finished && builder.HasLocalWork()) {
      finish_current_segment(QueryHorizon::ForPassthrough());
    }
    return root;
  }

  QueryHorizon BuildProjectionBody(const ast::ProjectionBody &body,
                                   ProjectionPosition projection_position) {
    CHECK(!body.star, common::InvalidArgumentError,
          Unsupported("projection star before rewrite"));

    ProjectionParts parts;
    parts.distinct = body.distinct;
    parts.position = projection_position;

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
      AppendNestedIRExpressions(
          &parts.nested_expressions,
          CollectNestedIRExpressions(projection_item.expression));
      parts.items.push_back(std::move(projection_item));
      const ProjectionItem &stored_item = parts.items.back();
      parts.interesting_order.reverse_projection.push_back(
          {.projected_alias = stored_item.alias,
           .source_expression = stored_item.expression});
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
      AppendNestedIRExpressions(
          &parts.nested_expressions,
          CollectNestedIRExpressions(order_item.expression));
      parts.required_order.items.emplace_back(order_item);
    }
    parts.interesting_order.required_order = parts.required_order;
    parts.interesting_order.candidates = parts.required_order.items;

    parts.skip = body.skip.get();
    parts.limit = body.limit.get();
    AppendNestedIRExpressions(&parts.nested_expressions,
                              CollectNestedIRExpressions(parts.skip));
    AppendNestedIRExpressions(&parts.nested_expressions,
                              CollectNestedIRExpressions(parts.limit));

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

  ProcedureCallHorizon BuildProcedureCallHorizon(
      const ast::StandaloneCall &call, bool expand_implicit_yields) {
    ProcedureCallHorizon horizon;
    horizon.procedure_name = call.procedure_name;
    horizon.yield_star = call.yield_star;
    horizon.arguments.reserve(call.arguments.size());
    for (const auto &argument : call.arguments) {
      CHECK(argument != nullptr, common::InvalidArgumentError,
            Missing("procedure argument"));
      horizon.arguments.push_back(argument.get());
    }
    horizon.yield_items = BuildProcedureYieldItems(
        call.procedure_name, call.yield_items,
        expand_implicit_yields &&
            (call.yield_star || call.yield_items.empty()));
    horizon.read_only = SemanticTableRef()
                            .KnownProcedureReadOnly(call.procedure_name)
                            .value_or(false);
    if (call.yield_where != nullptr) {
      AddProcedureYieldSelections(call.yield_where.get(), &horizon);
    }
    return horizon;
  }

  ProcedureCallHorizon BuildProcedureCallHorizon(const ast::InQueryCall &call) {
    ProcedureCallHorizon horizon;
    horizon.procedure_name = call.procedure_name;
    horizon.arguments.reserve(call.arguments.size());
    for (const auto &argument : call.arguments) {
      CHECK(argument != nullptr, common::InvalidArgumentError,
            Missing("procedure argument"));
      horizon.arguments.push_back(argument.get());
    }
    horizon.yield_items =
        BuildProcedureYieldItems(call.procedure_name, call.yield_items,
                                 /*expand_implicit_yields=*/false);
    horizon.read_only = SemanticTableRef()
                            .KnownProcedureReadOnly(call.procedure_name)
                            .value_or(false);
    if (call.yield_where != nullptr) {
      AddProcedureYieldSelections(call.yield_where.get(), &horizon);
    }
    return horizon;
  }

  std::vector<ProcedureYieldItem> BuildProcedureYieldItems(
      std::string_view procedure_name,
      const std::vector<ast::StandaloneCall::YieldItem> &items,
      bool expand_implicit_yields) const {
    std::vector<ProcedureYieldItem> out;
    if (!items.empty()) {
      out.reserve(items.size());
      for (const auto &item : items) {
        out.push_back(
            {.result_field = item.result_field, .variable = item.variable});
      }
      return out;
    }
    if (!expand_implicit_yields) {
      return out;
    }
    const std::vector<std::string> fields =
        SemanticTableRef().KnownProcedureYieldFields(procedure_name);
    out.reserve(fields.size());
    for (const auto &field : fields) {
      out.push_back({.result_field = field, .variable = field});
    }
    return out;
  }

  void AddProcedureYieldSelections(const ast::Expression *where,
                                   ProcedureCallHorizon *horizon) {
    CHECK(horizon != nullptr, common::InternalError,
          "procedure call horizon is null");
    std::unordered_set<std::string> selection_keys;
    AddSelectionPredicates(where, SemanticTableRef(), nullptr,
                           &horizon->yield_selections, &selection_keys);
  }

  const ast::SemanticTable *semantic_table_ = nullptr;
  std::size_t nested_expression_id_ = 0;
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
