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
#include "ir/query_ir.h"
#include "ir/query_ir_arguments.h"
#include "ir/query_ir_graph_builder.h"
#include "ir/query_ir_internal.h"

namespace ir {
namespace {

std::string RewrittenAstRequired(std::string_view detail) {
  std::string out =
      "CreateQueryIR requires AST rewritten by "
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

class QueryIRInputContractChecker final : public ast::ASTConstWalker {
 public:
  void Check(const ast::Statement &statement) { statement.Accept(*this); }

 protected:
  void Visit(const ast::ProjectionBody &node) override {
    RG_CHECK(!node.star, common::ErrorCode::InvalidParameter,
             RewrittenAstRequired("projection star must be expanded"));
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::ProjectionItem &node) override {
    RG_CHECK(node.expression != nullptr, common::ErrorCode::InvalidParameter,
             RewrittenAstRequired("projection item expression must exist"));
    RG_CHECK(!node.alias.empty(), common::ErrorCode::InvalidParameter,
             RewrittenAstRequired("projection item alias must be filled"));
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::ComparisonChainExpression &node) override {
    (void)node;
    RG_THROW(common::ErrorCode::InvalidParameter,
             RewrittenAstRequired(
                 "comparison chains must be rewritten to binary comparisons"));
  }

  void Visit(const ast::ParenthesizedExpression &node) override {
    (void)node;
    RG_THROW(
        common::ErrorCode::InvalidParameter,
        RewrittenAstRequired("parenthesized expressions must be unwrapped"));
  }

  void Visit(const ast::PatternPredicateExpression &node) override {
    (void)node;
    RG_THROW(
        common::ErrorCode::InvalidParameter,
        RewrittenAstRequired(
            "pattern predicates must be rewritten to existential subqueries"));
  }

  void Visit(const ast::ExistentialSubquery &node) override {
    RG_CHECK(node.pattern == nullptr, common::ErrorCode::InvalidParameter,
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
      RG_CHECK(!node.variable.empty(), common::ErrorCode::InvalidParameter,
               RewrittenAstRequired("anonymous nodes must be named"));
    }
    if (pattern_context_ == PatternContractContext::kRead) {
      RG_CHECK(node.labels.empty(), common::ErrorCode::InvalidParameter,
               RewrittenAstRequired(
                   "inline node labels in read patterns must be normalized"));
      RG_CHECK(
          !node.properties, common::ErrorCode::InvalidParameter,
          RewrittenAstRequired(
              "inline node properties in read patterns must be normalized"));
    }
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::RelationshipPattern &node) override {
    if (pattern_context_ != PatternContractContext::kNone) {
      RG_CHECK(node.detail != nullptr, common::ErrorCode::InvalidParameter,
               RewrittenAstRequired("anonymous relationships must be named"));
    }
    ast::ASTConstWalker::Visit(node);
  }

  void Visit(const ast::RelationshipDetail &node) override {
    if (pattern_context_ != PatternContractContext::kNone) {
      RG_CHECK(!node.variable.empty(), common::ErrorCode::InvalidParameter,
               RewrittenAstRequired("anonymous relationships must be named"));
    }
    if (pattern_context_ == PatternContractContext::kRead) {
      RG_CHECK(!node.properties, common::ErrorCode::InvalidParameter,
               RewrittenAstRequired(
                   "inline relationship properties in read patterns must be "
                   "normalized"));
    }
    ast::ASTConstWalker::Visit(node);
  }

 private:
  PatternContractContext pattern_context_ = PatternContractContext::kNone;
};

void CheckQueryIRInputContract(const ast::Statement &statement) {
  QueryIRInputContractChecker checker;
  checker.Check(statement);
}

class QueryIRBuilder {
 public:
  explicit QueryIRBuilder(const ast::SemanticTable &semantic_table)
      : semantic_table_(&semantic_table) {}

  std::unique_ptr<QueryIR> Build(const ast::Statement &statement) {
    CheckQueryIRInputContract(statement);
    switch (statement.node_type) {
      case ast::ASTNodeType::kRegularQuery: {
        std::unique_ptr<QueryIR> query_ir =
            BuildRegularQuery(ast::CastAst<ast::RegularQuery>(statement),
                              ProjectionPosition::kFinal);
        FinalizeQueryIRArguments(*query_ir, SemanticTableRef());
        return query_ir;
      }
      case ast::ASTNodeType::kStandaloneCall: {
        std::unique_ptr<QueryIR> query_ir =
            BuildStandaloneCall(ast::CastAst<ast::StandaloneCall>(statement));
        FinalizeQueryIRArguments(*query_ir, SemanticTableRef());
        return query_ir;
      }
      default: {
        RG_THROW(common::ErrorCode::InvalidParameter,
                 Unsupported("query type"));
      }
    }
  }

 private:
  class QuerySegmentBuilder {
   public:
    QuerySegmentBuilder(QueryIRBuilder &query_builder,
                        const std::unordered_set<std::string> &argument_ids)
        : query_builder_(query_builder),
          current_argument_ids_(argument_ids),
          builder_(query_builder.SemanticTableRef(), current_argument_ids_) {}

    void Finish(QueryHorizon horizon) {
      current_segment_->query_graph = builder_.Release();
      query_builder_.AttachQueryGraphSubqueries(&current_segment_->query_graph);
      current_segment_->horizon = std::move(horizon);
      current_segment_finished_ = true;
    }

    void StartNext() {
      current_argument_ids_ = SingleQueryIROutputSymbols(*current_segment_);
      current_segment_->tail = std::make_unique<SingleQueryIR>();
      current_segment_ = current_segment_->tail.get();
      builder_ = QueryGraphBuilder(query_builder_.SemanticTableRef(),
                                   current_argument_ids_);
      current_segment_finished_ = false;
    }

    void FinishAndStartNext(QueryHorizon horizon) {
      Finish(std::move(horizon));
      StartNext();
    }

    void BuildReadingClauses(
        const std::vector<std::unique_ptr<ast::ReadingClause>> &reading) {
      for (const auto &clause : reading) {
        RG_CHECK(clause != nullptr, common::ErrorCode::InvalidParameter,
                 Missing("reading clause"));
        switch (clause->node_type) {
          case ast::ASTNodeType::kMatch:
            builder_.BuildMatch(*ast::CastAst<ast::Match>(clause.get()));
            break;
          case ast::ASTNodeType::kUnwind:
            FinishAndStartNext(
                QueryHorizon::ForUnwind(query_builder_.BuildUnwindHorizon(
                    *ast::CastAst<ast::Unwind>(clause.get()))));
            break;
          case ast::ASTNodeType::kInQueryCall:
            FinishAndStartNext(QueryHorizon::ForProcedureCall(
                query_builder_.BuildProcedureCallHorizon(
                    *ast::CastAst<ast::InQueryCall>(clause.get()))));
            break;
          default:
            RG_THROW(common::ErrorCode::InvalidParameter,
                     Unsupported("reading clause"));
        }
      }
    }

    void BuildUpdatingClauses(
        const std::vector<std::unique_ptr<ast::UpdatingClause>> &updating,
        bool has_following_work) {
      for (std::size_t i = 0; i < updating.size(); ++i) {
        const auto &clause = updating[i];
        RG_CHECK(clause != nullptr, common::ErrorCode::InvalidParameter,
                 Missing("updating clause"));
        const bool is_merge = clause->Is(ast::ASTNodeType::kMerge);
        if (is_merge && builder_.HasLocalWork()) {
          FinishAndStartNext(QueryHorizon::ForPassthrough());
        }
        builder_.BuildUpdatingClause(*clause);
        if (is_merge) {
          Finish(QueryHorizon::ForPassthrough());
          if (i + 1 < updating.size() || has_following_work) {
            StartNext();
          }
        }
      }
    }

    [[nodiscard]] bool CanInlineWith(const ast::With &with_clause,
                                     bool has_update_boundary) const {
      return query_builder_.CanInlineWith(
          with_clause, builder_.AvailableSymbols(),
          builder_.HasOptionalMatches(), has_update_boundary);
    }

    void AddWhere(const ast::Expression *where) { builder_.AddWhere(where); }

    [[nodiscard]] bool HasLocalWork() const { return builder_.HasLocalWork(); }

    [[nodiscard]] bool ContainsUpdates() const {
      return builder_.ContainsUpdates();
    }

    [[nodiscard]] bool IsFinished() const { return current_segment_finished_; }

    SingleQueryIR TakeRoot() { return std::move(root_); }

    void FinishWithProjection(const ast::ProjectionClause *projection,
                              ProjectionPosition projection_position) {
      Finish(query_builder_.BuildProjectionClause(projection,
                                                  projection_position));
    }

   private:
    QueryIRBuilder &query_builder_;
    SingleQueryIR root_;
    SingleQueryIR *current_segment_ = &root_;
    std::unordered_set<std::string> current_argument_ids_;
    QueryGraphBuilder builder_;
    bool current_segment_finished_ = false;
  };

  [[nodiscard]] const ast::SemanticTable &SemanticTableRef() const {
    RG_CHECK(semantic_table_ != nullptr, common::ErrorCode::InternalError,
             "semantic table is null");
    return *semantic_table_;
  }

  std::unique_ptr<QueryIR> BuildRegularQuery(
      const ast::RegularQuery &query, ProjectionPosition projection_position) {
    RG_CHECK(query.single_query, common::ErrorCode::InvalidParameter,
             Missing("single query"));

    const ProjectionPosition branch_position =
        query.unions.empty() ? projection_position
                             : ProjectionPosition::kIntermediate;
    std::unique_ptr<QueryIR> query_ir = MakeSingleQueryIR(
        BuildSingleQuery(*query.single_query, branch_position));
    for (const auto &part : query.unions) {
      RG_CHECK(part && part->query, common::ErrorCode::InvalidParameter,
               Missing("UNION branch query"));
      SingleQueryIR rhs =
          BuildSingleQuery(*part->query, ProjectionPosition::kIntermediate);
      const std::vector<std::string> lhs_columns =
          QueryIROutputAliases(*query_ir);
      const std::vector<std::string> rhs_columns =
          SingleQueryIROutputAliases(rhs);
      std::unique_ptr<QueryIR> union_query =
          MakeUnionQueryIR(std::move(query_ir), std::move(rhs), part->all);
      union_query->RequireUnion().mappings =
          BuildUnionMappings(lhs_columns, rhs_columns);
      query_ir = std::move(union_query);
    }

    return query_ir;
  }

  std::unique_ptr<QueryIR> BuildStandaloneCall(
      const ast::StandaloneCall &call) {
    SingleQueryIR query;
    query.horizon = QueryHorizon::ForProcedureCall(
        BuildProcedureCallHorizon(call, /*expand_implicit_yields=*/true));
    AttachQueryHorizonSubqueries(&query.horizon);
    return MakeSingleQueryIR(std::move(query));
  }

  void AttachQueryGraphSubqueries(QueryGraph *query_graph) {
    RG_CHECK(query_graph != nullptr, common::ErrorCode::InternalError,
             "query graph is null");
    AttachSelectionSubqueries(&query_graph->selections);
    for (auto &optional_match : query_graph->optional_matches) {
      AttachQueryGraphSubqueries(&optional_match);
    }
  }

  void AttachQueryHorizonSubqueries(QueryHorizon *horizon) {
    RG_CHECK(horizon != nullptr, common::ErrorCode::InternalError,
             "query horizon is null");
    switch (horizon->kind) {
      case QueryHorizonKind::kRegularProjection:
        AttachSelectionSubqueries(
            &horizon->RequireRegularProjection().selections);
        AttachSubqueryIRExpressions(
            &horizon->RequireRegularProjection().subquery_expressions);
        return;
      case QueryHorizonKind::kDistinctProjection:
        AttachSelectionSubqueries(
            &horizon->RequireDistinctProjection().selections);
        AttachSubqueryIRExpressions(
            &horizon->RequireDistinctProjection().subquery_expressions);
        return;
      case QueryHorizonKind::kAggregatingProjection:
        AttachSelectionSubqueries(
            &horizon->RequireAggregatingProjection().selections);
        AttachSubqueryIRExpressions(
            &horizon->RequireAggregatingProjection().subquery_expressions);
        return;
      case QueryHorizonKind::kProcedureCall:
        AttachSelectionSubqueries(
            &horizon->RequireProcedureCall().yield_selections);
        return;
      case QueryHorizonKind::kUnwind:
      case QueryHorizonKind::kPassthrough:
        return;
    }
    RG_THROW(common::ErrorCode::InternalError, "unknown query horizon kind");
  }

  void AttachSelectionSubqueries(Selections *selections) {
    RG_CHECK(selections != nullptr, common::ErrorCode::InternalError,
             "selections is null");
    for (auto &predicate_ptr : selections->predicates) {
      auto &predicate = *predicate_ptr;
      AppendSubqueryIRExpressions(
          &predicate.subquery_expressions,
          CollectSubqueryIRExpressions(predicate.expression));
      if (predicate.GetIf<ExistsPredicate>() == nullptr) {
        continue;
      }
      RG_CHECK(predicate.Subquery() != nullptr,
               common::ErrorCode::InvalidParameter,
               Missing("EXISTS subquery IR expression"));
    }
  }

  void AttachSubqueryIRExpressions(
      SubqueryIRExpressions *subquery_expressions) {
    RG_CHECK(subquery_expressions != nullptr, common::ErrorCode::InternalError,
             "subquery expression list is null");
    for (auto &nested : *subquery_expressions) {
      RG_CHECK(nested != nullptr, common::ErrorCode::InternalError,
               "subquery IR expression is null");
      RG_CHECK(nested->query != nullptr, common::ErrorCode::InvalidParameter,
               "subquery IR expression query is null");
    }
  }

  static void AppendSubqueryIRExpressions(SubqueryIRExpressions *target,
                                          SubqueryIRExpressions incoming) {
    RG_CHECK(target != nullptr, common::ErrorCode::InternalError,
             "subquery expression target is null");
    target->reserve(target->size() + incoming.size());
    for (auto &nested : incoming) {
      RG_CHECK(nested != nullptr, common::ErrorCode::InternalError,
               "subquery IR expression is null");
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

  SubqueryIRExpressions CollectSubqueryIRExpressions(
      const ast::Expression *expression) {
    SubqueryIRExpressions subquery_expressions;
    if (expression == nullptr) {
      return subquery_expressions;
    }

    class Collector final : public ast::ASTConstWalker {
     public:
      Collector(QueryIRBuilder *builder,
                SubqueryIRExpressions *subquery_expressions)
          : builder_(builder), subquery_expressions_(subquery_expressions) {}

     protected:
      void Visit(const ast::ListComprehension &node) override {
        WalkMaybe(node.list_expr);
        local_variables_.push_back(node.variable);
        WalkMaybe(node.where_expr);
        WalkMaybe(node.eval_expr);
        local_variables_.pop_back();
      }

      void Visit(const ast::ReduceExpression &node) override {
        WalkMaybe(node.initial);
        WalkMaybe(node.list_expr);
        local_variables_.push_back(node.accumulator);
        local_variables_.push_back(node.variable);
        WalkMaybe(node.eval_expr);
        local_variables_.pop_back();
        local_variables_.pop_back();
      }

      void Visit(const ast::ExistentialSubquery &node) override {
        const auto dependencies =
            builder_->SemanticTableRef().ExpressionDependencies(node);
        const bool locally_correlated =
            std::any_of(local_variables_.begin(), local_variables_.end(),
                        [&](const std::string &variable) {
                          return dependencies.contains(variable);
                        });
        if (!locally_correlated) {
          subquery_expressions_->push_back(
              builder_->BuildExistsIRExpression(node));
        }
      }

      void Visit(const ast::PatternComprehension &node) override {
        const auto dependencies =
            builder_->SemanticTableRef().ExpressionDependencies(node);
        const bool locally_correlated =
            std::any_of(local_variables_.begin(), local_variables_.end(),
                        [&](const std::string &variable) {
                          return dependencies.contains(variable);
                        });
        if (!locally_correlated) {
          subquery_expressions_->push_back(
              builder_->BuildListIRExpression(node));
        }
        WalkMaybe(node.where_expr);
        WalkMaybe(node.eval_expr);
      }

     private:
      QueryIRBuilder *builder_ = nullptr;
      SubqueryIRExpressions *subquery_expressions_ = nullptr;
      std::vector<std::string> local_variables_;
    };

    Collector collector(this, &subquery_expressions);
    expression->Accept(collector);
    return subquery_expressions;
  }

  std::unique_ptr<ExistsIRExpression> BuildExistsIRExpression(
      const ast::ExistentialSubquery &exists) {
    RG_CHECK(exists.query != nullptr, common::ErrorCode::InternalError,
             "EXISTS subquery must contain a regular query");
    auto nested = std::make_unique<ExistsIRExpression>();
    nested->expression = &exists;
    nested->dependencies = SemanticTableRef().ExpressionDependencies(exists);
    nested->value_variable = NextNestedVariable("exists");
    nested->query =
        BuildRegularQuery(*exists.query, ProjectionPosition::kIntermediate);
    return nested;
  }

  std::unique_ptr<ListIRExpression> BuildListIRExpression(
      const ast::PatternComprehension &comprehension) {
    RG_CHECK(comprehension.relationships_pattern != nullptr,
             common::ErrorCode::InvalidParameter,
             Missing("pattern comprehension relationships pattern"));
    RG_CHECK(comprehension.eval_expr != nullptr,
             common::ErrorCode::InvalidParameter,
             Missing("pattern comprehension eval expression"));

    auto nested = std::make_unique<ListIRExpression>();
    nested->expression = &comprehension;
    nested->dependencies =
        SemanticTableRef().ExpressionDependencies(comprehension);
    nested->value_variable = NextNestedVariable("list_value");
    nested->collection_variable = comprehension.variable.empty()
                                      ? NextNestedVariable("list")
                                      : comprehension.variable;

    SingleQueryIR query;
    AddRelationshipsPatternToQueryGraph(&query.query_graph,
                                        *comprehension.relationships_pattern,
                                        comprehension.variable);
    if (comprehension.where_expr != nullptr) {
      AddSelectionPredicates(comprehension.where_expr.get(), SemanticTableRef(),
                             &query.query_graph, &query.query_graph.selections);
    }
    AttachQueryGraphSubqueries(&query.query_graph);

    RegularQueryProjection projection;
    projection.position = ProjectionPosition::kIntermediate;
    projection.items.push_back({.expression = comprehension.eval_expr.get(),
                                .alias = nested->value_variable});
    AppendSubqueryIRExpressions(
        &projection.subquery_expressions,
        CollectSubqueryIRExpressions(comprehension.eval_expr.get()));
    query.horizon = QueryHorizon::ForRegularProjection(std::move(projection));
    AttachQueryHorizonSubqueries(&query.horizon);
    nested->query = MakeSingleQueryIR(std::move(query));
    return nested;
  }

  [[nodiscard]] bool IsPureVariablePassthrough(
      const ast::ProjectionBody &body,
      const std::unordered_set<std::string> &available_symbols) const {
    if (body.star || body.distinct || !body.order_by.empty() || body.skip ||
        body.limit) {
      return false;
    }

    std::unordered_set<std::string> projected_symbols;
    projected_symbols.reserve(body.items.size());
    for (const auto &item : body.items) {
      RG_CHECK(item != nullptr, common::ErrorCode::InvalidParameter,
               "projection item is null");
      RG_CHECK(item->expression != nullptr, common::ErrorCode::InvalidParameter,
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

  [[nodiscard]] bool CanInlineWith(
      const ast::With &with_clause,
      const std::unordered_set<std::string> &available_symbols,
      bool has_optional_boundary, bool has_update_boundary) const {
    if (has_optional_boundary || has_update_boundary) {
      return false;
    }
    RG_CHECK(with_clause.body != nullptr, common::ErrorCode::InvalidParameter,
             Missing("WITH body"));
    return IsPureVariablePassthrough(*with_clause.body, available_symbols);
  }

  SingleQueryIR BuildSingleQuery(const ast::SingleQuery &query,
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
        RG_THROW(common::ErrorCode::InvalidParameter,
                 Unsupported("single query type"));
      }
    }
  }

  SingleQueryIR BuildSinglePartQuery(const ast::SinglePartQuery &query,
                                     ProjectionPosition projection_position) {
    RG_CHECK(query.return_clause || !query.updating_clauses.empty(),
             common::ErrorCode::InvalidParameter, Missing("RETURN clause"));
    QuerySegmentBuilder segment_builder(*this, {});
    segment_builder.BuildReadingClauses(query.reading_clauses);
    segment_builder.BuildUpdatingClauses(query.updating_clauses,
                                         query.return_clause != nullptr);
    if (query.return_clause != nullptr) {
      segment_builder.FinishWithProjection(query.return_clause.get(),
                                           projection_position);
    } else if (!segment_builder.IsFinished() &&
               segment_builder.HasLocalWork()) {
      segment_builder.Finish(QueryHorizon::ForPassthrough());
    }
    return segment_builder.TakeRoot();
  }

  SingleQueryIR BuildMultiPartQuery(const ast::MultiPartQuery &query,
                                    ProjectionPosition projection_position) {
    RG_CHECK(query.final_single_part_query, common::ErrorCode::InvalidParameter,
             Missing("final single query"));

    QuerySegmentBuilder segment_builder(*this, {});

    for (const auto &part : query.parts) {
      RG_CHECK(part.with_clause, common::ErrorCode::InvalidParameter,
               Missing("WITH clause"));
      RG_CHECK(part.with_clause->body != nullptr,
               common::ErrorCode::InvalidParameter, Missing("WITH body"));

      segment_builder.BuildReadingClauses(part.reading_clauses);
      segment_builder.BuildUpdatingClauses(part.updating_clauses,
                                           /*has_following_work=*/true);

      const ast::With &with_clause = *part.with_clause;
      if (segment_builder.CanInlineWith(
              with_clause, !part.updating_clauses.empty() ||
                               segment_builder.ContainsUpdates())) {
        if (with_clause.where != nullptr) {
          segment_builder.AddWhere(with_clause.where.get());
        }
        continue;
      }

      segment_builder.FinishAndStartNext(BuildProjectionClause(
          &with_clause, ProjectionPosition::kIntermediate));
    }

    const ast::SinglePartQuery &final = *query.final_single_part_query;
    RG_CHECK(final.return_clause || !final.updating_clauses.empty(),
             common::ErrorCode::InvalidParameter, Missing("RETURN clause"));
    segment_builder.BuildReadingClauses(final.reading_clauses);
    segment_builder.BuildUpdatingClauses(final.updating_clauses,
                                         final.return_clause != nullptr);
    if (final.return_clause != nullptr) {
      segment_builder.FinishWithProjection(final.return_clause.get(),
                                           projection_position);
    } else if (!segment_builder.IsFinished() &&
               segment_builder.HasLocalWork()) {
      segment_builder.Finish(QueryHorizon::ForPassthrough());
    }
    return segment_builder.TakeRoot();
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
    SubqueryIRExpressions subquery_expressions;
  };

  static void MoveProjectionTail(ProjectionParts *parts,
                                 QueryProjection *projection) {
    RG_CHECK(parts != nullptr, common::ErrorCode::InternalError,
             "projection parts is null");
    RG_CHECK(projection != nullptr, common::ErrorCode::InternalError,
             "projection is null");
    projection->required_order = std::move(parts->required_order);
    projection->interesting_order = std::move(parts->interesting_order);
    projection->pagination.skip = parts->skip;
    projection->pagination.limit = parts->limit;
    projection->position = parts->position;
    projection->subquery_expressions = std::move(parts->subquery_expressions);
  }

  void AddProjectionSelections(QueryHorizon *horizon,
                               const ast::Expression *where) {
    RG_CHECK(horizon != nullptr, common::ErrorCode::InternalError,
             "query horizon is null");
    if (where == nullptr) {
      return;
    }
    switch (horizon->kind) {
      case QueryHorizonKind::kRegularProjection:
        AddSelectionPredicates(where, SemanticTableRef(), nullptr,
                               &horizon->RequireRegularProjection().selections);
        return;
      case QueryHorizonKind::kDistinctProjection:
        AddSelectionPredicates(
            where, SemanticTableRef(), nullptr,
            &horizon->RequireDistinctProjection().selections);
        return;
      case QueryHorizonKind::kAggregatingProjection:
        AddSelectionPredicates(
            where, SemanticTableRef(), nullptr,
            &horizon->RequireAggregatingProjection().selections);
        return;
      case QueryHorizonKind::kUnwind:
        RG_THROW(common::ErrorCode::InternalError,
                 "UNWIND horizon cannot have projection WHERE");
      case QueryHorizonKind::kProcedureCall:
        RG_THROW(common::ErrorCode::InternalError,
                 "procedure call horizon cannot have projection WHERE");
      case QueryHorizonKind::kPassthrough:
        RG_THROW(common::ErrorCode::InternalError,
                 "passthrough horizon cannot have projection WHERE");
    }
    RG_THROW(common::ErrorCode::InternalError, "unknown query horizon kind");
  }

  QueryHorizon BuildProjectionClause(
      const ast::ProjectionClause *projection_clause,
      ProjectionPosition projection_position) {
    RG_CHECK(projection_clause != nullptr, common::ErrorCode::InvalidParameter,
             Missing("projection clause"));
    RG_CHECK(projection_clause->body, common::ErrorCode::InvalidParameter,
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

  QueryHorizon BuildProjectionBody(const ast::ProjectionBody &body,
                                   ProjectionPosition projection_position) {
    RG_CHECK(!body.star, common::ErrorCode::InvalidParameter,
             Unsupported("projection star before rewrite"));

    ProjectionParts parts;
    parts.distinct = body.distinct;
    parts.position = projection_position;

    parts.items.reserve(body.items.size());
    parts.grouping_items.reserve(body.items.size());
    parts.aggregation_items.reserve(body.items.size());
    for (const auto &item : body.items) {
      RG_CHECK(item, common::ErrorCode::InvalidParameter,
               "null projection item is not supported");
      RG_CHECK(item->expression, common::ErrorCode::InvalidParameter,
               Missing("projection item expression"));
      ProjectionItem projection_item;
      projection_item.expression = item->expression.get();
      projection_item.alias = item->alias;
      RG_CHECK(!projection_item.alias.empty(),
               common::ErrorCode::InvalidParameter,
               "projection item alias is empty after rewrite");
      const auto &output_types = SemanticTableRef().ProjectionOutputTypes(body);
      const auto output_type = output_types.find(projection_item.alias);
      if (output_type != output_types.end()) {
        projection_item.semantic_type = output_type->second;
      }
      AppendSubqueryIRExpressions(
          &parts.subquery_expressions,
          CollectSubqueryIRExpressions(projection_item.expression));
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
      RG_CHECK(item, common::ErrorCode::InvalidParameter,
               "null sort item is not supported");
      RG_CHECK(item->expression, common::ErrorCode::InvalidParameter,
               Missing("sort expression"));
      OrderItem order_item;
      order_item.expression = item->expression.get();
      order_item.direction = item->ascending ? OrderDirection::kAscending
                                             : OrderDirection::kDescending;
      AppendSubqueryIRExpressions(
          &parts.subquery_expressions,
          CollectSubqueryIRExpressions(order_item.expression));
      parts.required_order.items.emplace_back(order_item);
    }
    parts.interesting_order.required_order = parts.required_order;
    parts.interesting_order.candidates = parts.required_order.items;

    parts.skip = body.skip.get();
    parts.limit = body.limit.get();
    AppendSubqueryIRExpressions(&parts.subquery_expressions,
                                CollectSubqueryIRExpressions(parts.skip));
    AppendSubqueryIRExpressions(&parts.subquery_expressions,
                                CollectSubqueryIRExpressions(parts.limit));

    if (!parts.aggregation_items.empty()) {
      AggregatingQueryProjection projection;
      projection.items = std::move(parts.items);
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
    RG_CHECK(unwind.expression != nullptr, common::ErrorCode::InvalidParameter,
             Missing("UNWIND expression"));
    RG_CHECK(!unwind.variable.empty(), common::ErrorCode::InvalidParameter,
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
      RG_CHECK(argument != nullptr, common::ErrorCode::InvalidParameter,
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
      RG_CHECK(argument != nullptr, common::ErrorCode::InvalidParameter,
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

  [[nodiscard]] std::vector<ProcedureYieldItem> BuildProcedureYieldItems(
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
    RG_CHECK(horizon != nullptr, common::ErrorCode::InternalError,
             "procedure call horizon is null");
    AddSelectionPredicates(where, SemanticTableRef(), nullptr,
                           &horizon->yield_selections);
  }

  const ast::SemanticTable *semantic_table_ = nullptr;
  std::size_t nested_expression_id_ = 0;
};

}  // namespace

std::unique_ptr<QueryIR> BuildQueryIR(
    const ast::Statement &statement, const ast::SemanticTable &semantic_table) {
  QueryIRBuilder builder(semantic_table);
  return builder.Build(statement);
}

}  // namespace ir
