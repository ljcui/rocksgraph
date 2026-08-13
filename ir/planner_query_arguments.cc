#include "ir/planner_query_arguments.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/semantic_table.h"
#include "common/exception.h"
#include "ir/planner_query_internal.h"

namespace ir {
namespace {

std::unordered_set<std::string> ProjectionItemAliases(
    const std::vector<ProjectionItem> &items) {
  std::unordered_set<std::string> symbols;
  for (const auto &item : items) {
    AddSymbol(&symbols, item.alias);
  }
  return symbols;
}

std::vector<std::string> ProjectionItemAliasList(
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

std::vector<std::string> SortedSymbolList(
    const std::unordered_set<std::string> &symbols) {
  std::vector<std::string> out(symbols.begin(), symbols.end());
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> InternalSinglePlannerQueryOutputAliases(
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
      return ProjectionItemAliasList(
          last->horizon.RequireAggregatingProjection().items);
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

class PlannerQueryArgumentFinalizer {
 public:
  explicit PlannerQueryArgumentFinalizer(
      const ast::SemanticTable &semantic_table)
      : semantic_table_(&semantic_table) {}

  void Finalize(PlannerQuery &query) const {
    FinalizePlannerQueryArguments(query, {});
  }

 private:
  [[nodiscard]] const ast::SemanticTable &SemanticTableRef() const {
    CHECK(semantic_table_ != nullptr, common::InternalError,
          "semantic table is null");
    return *semantic_table_;
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
      if (segment->horizon.kind == QueryHorizonKind::kUnwind ||
          segment->horizon.kind == QueryHorizonKind::kProcedureCall ||
          segment->horizon.kind == QueryHorizonKind::kPassthrough) {
        AddSymbols(&segment->query_graph.argument_ids, available_symbols);
      }
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

  [[nodiscard]] std::unordered_set<std::string> SinglePlannerQueryDependencies(
      const SinglePlannerQuery &query) const {
    std::unordered_set<std::string> dependencies =
        QueryGraphDependencies(query.query_graph);
    AddSymbols(&dependencies, QueryHorizonDependencies(query.horizon));
    return dependencies;
  }

  [[nodiscard]] std::unordered_set<std::string> QueryGraphDependencies(
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

  [[nodiscard]] std::unordered_set<std::string> QueryHorizonDependencies(
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

  const ast::SemanticTable *semantic_table_ = nullptr;
};

}  // namespace

void FinalizePlannerQueryArguments(PlannerQuery &query,
                                   const ast::SemanticTable &semantic_table) {
  PlannerQueryArgumentFinalizer finalizer(semantic_table);
  finalizer.Finalize(query);
}

std::unordered_set<std::string> SinglePlannerQueryOutputSymbols(
    const SinglePlannerQuery &query) {
  switch (query.horizon.kind) {
    case QueryHorizonKind::kRegularProjection:
      return ProjectionItemAliases(
          query.horizon.RequireRegularProjection().items);
    case QueryHorizonKind::kDistinctProjection:
      return ProjectionItemAliases(
          query.horizon.RequireDistinctProjection().grouping_items);
    case QueryHorizonKind::kAggregatingProjection: {
      return ProjectionItemAliases(
          query.horizon.RequireAggregatingProjection().items);
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

std::vector<std::string> SinglePlannerQueryOutputAliases(
    const SinglePlannerQuery &query) {
  return InternalSinglePlannerQueryOutputAliases(query);
}

std::vector<std::string> PlannerQueryOutputAliases(const PlannerQuery &query) {
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

std::vector<UnionPlannerQuery::UnionMapping> BuildUnionMappings(
    const std::vector<std::string> &lhs_columns,
    const std::vector<std::string> &rhs_columns) {
  CHECK(lhs_columns.size() == rhs_columns.size(), common::InvalidArgumentError,
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

}  // namespace ir
