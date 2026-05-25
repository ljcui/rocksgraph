#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_const_walker.h"
#include "ast/semantic_table.h"
#include "common/exception.h"
#include "ir/planner_query.h"
#include "ir/planner_query_arguments.h"
#include "ir/planner_query_graph_builder.h"
#include "ir/planner_query_internal.h"

namespace ir {
namespace {

std::string RewrittenAstRequired(std::string_view detail) {
  std::string out =
      "CreatePlannerQuery requires AST rewritten by "
      "ast::ParseCypherAndRewrite: ";
  out.append(detail.data(), detail.size());
  return out;
}

enum class PatternContractContext { kNone, kRead, kUpdate };

class PatternContractScope {
 public:
  PatternContractScope(PatternContractContext *target,
                       PatternContractContext next)
      : target_(target), previous_(*target) {
    *target_ = next;
  }

  PatternContractScope(const PatternContractScope &) = delete;
  PatternContractScope &operator=(const PatternContractScope &) = delete;

  ~PatternContractScope() { *target_ = previous_; }

 private:
  PatternContractContext *target_ = nullptr;
  PatternContractContext previous_ = PatternContractContext::kNone;
};

class PlannerQueryInputContractChecker final : public ast::ASTConstWalker {
 public:
  void Check(const ast::Statement &statement) { statement.Accept(*this); }

 protected:
  void Visit(const ast::ProjectionBody &node) override {
    CHECK(!node.star, common::InvalidArgumentError,
          RewrittenAstRequired("projection star must be expanded"));
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::ProjectionItem &node) override {
    CHECK(node.expression != nullptr, common::InvalidArgumentError,
          RewrittenAstRequired("projection item expression must exist"));
    CHECK(!node.alias.empty(), common::InvalidArgumentError,
          RewrittenAstRequired("projection item alias must be filled"));
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::ComparisonChainExpression &node) override {
    (void)node;
    THROW(common::InvalidArgumentError,
          RewrittenAstRequired(
              "comparison chains must be rewritten to binary comparisons"));
  }

  void Visit(const ast::ParenthesizedExpression &node) override {
    (void)node;
    THROW(common::InvalidArgumentError,
          RewrittenAstRequired("parenthesized expressions must be unwrapped"));
  }

  void Visit(const ast::PatternPredicateExpression &node) override {
    (void)node;
    THROW(
        common::InvalidArgumentError,
        RewrittenAstRequired(
            "pattern predicates must be rewritten to existential subqueries"));
  }

  void Visit(const ast::ExistentialSubquery &node) override {
    CHECK(node.pattern == nullptr, common::InvalidArgumentError,
          RewrittenAstRequired(
              "EXISTS patterns must be rewritten to MATCH subqueries"));
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::Match &node) override {
    {
      PatternContractScope scope(&pattern_context_,
                                 PatternContractContext::kRead);
      WalkMaybe(node.pattern);
    }
    WalkMaybe(node.where);
  }

  void Visit(const ast::Create &node) override {
    PatternContractScope scope(&pattern_context_,
                               PatternContractContext::kUpdate);
    WalkMaybe(node.pattern);
  }

  void Visit(const ast::Merge &node) override {
    {
      PatternContractScope scope(&pattern_context_,
                                 PatternContractContext::kUpdate);
      WalkMaybe(node.pattern_part);
    }
    for (const auto &action : node.actions) {
      WalkMaybe(action.second);
    }
  }

  void Visit(const ast::PatternComprehension &node) override {
    {
      PatternContractScope scope(&pattern_context_,
                                 PatternContractContext::kRead);
      WalkMaybe(node.relationships_pattern);
    }
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
  }

  void Visit(const ast::NodePattern &node) override {
    if (pattern_context_ != PatternContractContext::kNone) {
      CHECK(!node.variable.empty(), common::InvalidArgumentError,
            RewrittenAstRequired("anonymous nodes must be named"));
    }
    if (pattern_context_ == PatternContractContext::kRead) {
      CHECK(node.labels.empty(), common::InvalidArgumentError,
            RewrittenAstRequired(
                "inline node labels in read patterns must be normalized"));
      CHECK(!node.properties, common::InvalidArgumentError,
            RewrittenAstRequired(
                "inline node properties in read patterns must be normalized"));
    }
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::RelationshipPattern &node) override {
    if (pattern_context_ != PatternContractContext::kNone) {
      CHECK(node.detail != nullptr, common::InvalidArgumentError,
            RewrittenAstRequired("anonymous relationships must be named"));
    }
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::RelationshipDetail &node) override {
    if (pattern_context_ != PatternContractContext::kNone) {
      CHECK(!node.variable.empty(), common::InvalidArgumentError,
            RewrittenAstRequired("anonymous relationships must be named"));
    }
    if (pattern_context_ == PatternContractContext::kRead) {
      CHECK(!node.properties, common::InvalidArgumentError,
            RewrittenAstRequired(
                "inline relationship properties in read patterns must be "
                "normalized"));
    }
    ast::ASTConstWalker::Visit(node);
  }

 private:
  PatternContractContext pattern_context_ = PatternContractContext::kNone;
};

void CheckPlannerQueryInputContract(const ast::Statement &statement) {
  PlannerQueryInputContractChecker checker;
  checker.Check(statement);
}

class PlannerQueryBuilder {
 public:
  explicit PlannerQueryBuilder(const ast::SemanticTable &semantic_table)
      : semantic_table_(&semantic_table) {}

  std::unique_ptr<PlannerQuery> Build(const ast::Statement &statement) {
    CheckPlannerQueryInputContract(statement);
    switch (statement.node_type) {
      case ast::ASTNodeType::kRegularQuery: {
        std::unique_ptr<PlannerQuery> planner_query =
            BuildRegularQuery(ast::CastAst<ast::RegularQuery>(statement),
                              ProjectionPosition::kFinal);
        FinalizePlannerQueryArguments(*planner_query, SemanticTableRef());
        return planner_query;
      }
      case ast::ASTNodeType::kStandaloneCall: {
        std::unique_ptr<PlannerQuery> planner_query =
            BuildStandaloneCall(ast::CastAst<ast::StandaloneCall>(statement));
        FinalizePlannerQueryArguments(*planner_query, SemanticTableRef());
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
      if (predicate.kind != PredicateKind::kExistsSubquery &&
          predicate.kind != PredicateKind::kNotExistsSubquery) {
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
    AddPatternToQueryGraph(&query.query_graph, *exists.pattern);
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
    AddRelationshipsPatternToQueryGraph(&query.query_graph,
                                        *comprehension.relationships_pattern);
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

std::unique_ptr<PlannerQuery> BuildPlannerQuery(
    const ast::Statement &statement, const ast::SemanticTable &semantic_table) {
  PlannerQueryBuilder builder(semantic_table);
  return builder.Build(statement);
}

}  // namespace ir
