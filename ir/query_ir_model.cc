#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "ir/query_ir.h"
#include "ir/query_ir_internal.h"

namespace ir {

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
    if (predicate.property_value == nullptr) {
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

const SingleQueryIR *SingleQueryIR::Last() const { return LastQueryPart(this); }

SingleQueryIR *SingleQueryIR::Last() { return LastQueryPart(this); }

const SingleQueryIR &QueryIR::RequireSingle() const {
  const auto *query = dynamic_cast<const SingleQueryIR *>(this);
  CHECK(query != nullptr, common::InvalidArgumentError,
        Unsupported("non-single query IR"));
  return *query;
}

SingleQueryIR &QueryIR::RequireSingle() {
  auto *query = dynamic_cast<SingleQueryIR *>(this);
  CHECK(query != nullptr, common::InvalidArgumentError,
        Unsupported("non-single query IR"));
  return *query;
}

const UnionQueryIR &QueryIR::RequireUnion() const {
  const auto *query = dynamic_cast<const UnionQueryIR *>(this);
  CHECK(query != nullptr, common::InvalidArgumentError,
        Unsupported("non-union query IR"));
  return *query;
}

UnionQueryIR &QueryIR::RequireUnion() {
  auto *query = dynamic_cast<UnionQueryIR *>(this);
  CHECK(query != nullptr, common::InvalidArgumentError,
        Unsupported("non-union query IR"));
  return *query;
}

std::unique_ptr<QueryIR> MakeSingleQueryIR(SingleQueryIR single_query) {
  return std::make_unique<SingleQueryIR>(std::move(single_query));
}

std::unique_ptr<QueryIR> MakeUnionQueryIR(std::unique_ptr<QueryIR> lhs,
                                          SingleQueryIR rhs, bool all) {
  CHECK(lhs != nullptr, common::InvalidArgumentError,
        "UNION lhs query IR is null");
  auto query = std::make_unique<UnionQueryIR>();
  query->lhs = std::move(lhs);
  query->rhs = std::move(rhs);
  query->all = all;
  query->distinct = !all;
  return query;
}

}  // namespace ir
