#include "ir/planner_query.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/expression_dependency.h"
#include "ast/expression_to_string.h"
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

std::unordered_set<std::string> QueryGraphAvailableSymbols(
    const QueryGraph &query_graph) {
  std::unordered_set<std::string> symbols = query_graph.argument_ids;
  symbols.insert(query_graph.pattern_nodes.begin(),
                 query_graph.pattern_nodes.end());
  for (const auto &relationship : query_graph.pattern_relationships) {
    CHECK(!relationship.variable.empty(), common::InvalidArgumentError,
          "relationship variable is empty");
    symbols.insert(relationship.variable);
  }
  return symbols;
}

std::unordered_set<std::string> ProjectionOutputSymbols(
    const Projection &projection) {
  std::unordered_set<std::string> symbols;
  for (const auto &item : projection.items) {
    CHECK(!item.alias.empty(), common::InvalidArgumentError,
          "projection item alias is empty");
    symbols.insert(item.alias);
  }
  return symbols;
}

}  // namespace

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
      THROW(common::InvalidArgumentError, Unsupported("named path"));
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
      std::unordered_set<std::string> argument_ids = {}) {
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

  void BuildMatch(const ast::Match &match) {
    if (match.optional_match) {
      THROW(common::InvalidArgumentError, Unsupported("OPTIONAL MATCH"));
    }
    CHECK(match.pattern != nullptr, common::InvalidArgumentError,
          Missing("MATCH pattern"));
    PatternConverter converter(&graph_);
    converter.AddPattern(*match.pattern);
    if (match.where) {
      AddWhere(match.where.get());
    }
  }

  void AddWhere(const ast::Expression *where) {
    CHECK(where != nullptr, common::InvalidArgumentError,
          "WHERE predicate is null");
    std::vector<const ast::Expression *> predicates;
    SplitConjunctivePredicates(where, &predicates);
    CHECK(!predicates.empty(), common::InvalidArgumentError,
          "WHERE predicate list is empty");
    for (const ast::Expression *predicate : predicates) {
      CHECK(predicate != nullptr, common::InvalidArgumentError,
            "null WHERE predicate is not supported");
      const std::string predicate_key = ast::ExpressionToString(*predicate);
      CHECK(!predicate_key.empty(), common::InvalidArgumentError,
            "failed to stringify WHERE predicate");
      if (!where_keys_.insert(predicate_key).second) {
        continue;
      }

      Predicate where_predicate;
      where_predicate.expression = predicate;
      where_predicate.dependencies =
          ast::CollectExpressionDependencies(*predicate, CoveredSymbols());
      ClassifyPredicate(&where_predicate);
      graph_.selections.predicates.push_back(std::move(where_predicate));
    }
  }

  QueryGraph Release() { return std::move(graph_); }

 private:
  [[nodiscard]] std::unordered_set<std::string> CoveredSymbols() const {
    std::unordered_set<std::string> symbols = graph_.argument_ids;
    symbols.insert(graph_.pattern_nodes.begin(), graph_.pattern_nodes.end());
    for (const auto &relationship : graph_.pattern_relationships) {
      CHECK(!relationship.variable.empty(), common::InvalidArgumentError,
            "relationship variable is empty");
      symbols.insert(relationship.variable);
    }
    return symbols;
  }

  [[nodiscard]] bool IsRelationshipVariable(const std::string &name) const {
    for (const auto &relationship : graph_.pattern_relationships) {
      if (relationship.variable == name) {
        return true;
      }
    }
    return false;
  }

  void ClassifyPredicate(Predicate *predicate) const {
    CHECK(predicate != nullptr, common::InternalError, "predicate is null");
    CHECK(predicate->expression != nullptr, common::InvalidArgumentError,
          "WHERE predicate expression is null");
    const ast::Expression *expression =
        UnwrapParenthesized(predicate->expression);
    CHECK(expression != nullptr, common::InvalidArgumentError,
          "WHERE predicate expression is null");

    if (expression->Is(ast::ASTNodeType::kLabelPredicateExpression)) {
      const auto *label =
          ast::CastAst<ast::LabelPredicateExpression>(expression);
      const ast::Variable *variable = AsVariableExpression(label->expr.get());
      if (variable == nullptr) {
        return;
      }
      predicate->variable = variable->name;
      if (IsRelationshipVariable(variable->name)) {
        predicate->kind = PredicateKind::kRelationshipType;
        predicate->relationship_types = label->labels;
      } else if (graph_.pattern_nodes.contains(variable->name)) {
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

    if (expression->Is(ast::ASTNodeType::kExistentialSubquery)) {
      predicate->kind = PredicateKind::kExistsSubquery;
    }
  }

  QueryGraph graph_;
  std::unordered_set<std::string> where_keys_;
};

QueryHorizon QueryHorizon::ForProjection(Projection projection) {
  QueryHorizon horizon;
  horizon.kind = QueryHorizonKind::kProjection;
  horizon.projection = std::move(projection);
  return horizon;
}

QueryHorizon QueryHorizon::ForUnwind(UnwindHorizon unwind) {
  QueryHorizon horizon;
  horizon.kind = QueryHorizonKind::kUnwind;
  horizon.unwind = std::move(unwind);
  return horizon;
}

const Projection &QueryHorizon::RequireProjection() const {
  CHECK(kind == QueryHorizonKind::kProjection, common::InvalidArgumentError,
        Unsupported("query horizon"));
  return projection;
}

Projection &QueryHorizon::RequireProjection() {
  CHECK(kind == QueryHorizonKind::kProjection, common::InvalidArgumentError,
        Unsupported("query horizon"));
  return projection;
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

void CheckNoUpdatingClauses(
    const std::vector<std::unique_ptr<ast::UpdatingClause>> &updating_clauses) {
  if (!updating_clauses.empty()) {
    THROW(common::InvalidArgumentError, Unsupported("updating clause"));
  }
}

class PlannerQueryBuilder {
 public:
  std::unique_ptr<PlannerQuery> Build(const ast::Statement &statement) {
    switch (statement.node_type) {
      case ast::ASTNodeType::kRegularQuery: {
        return BuildRegularQuery(ast::CastAst<ast::RegularQuery>(statement));
      }
      default: {
        THROW(common::InvalidArgumentError, Unsupported("query type"));
      }
    }
  }

 private:
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
    CheckNoUpdatingClauses(query.updating_clauses);
    CHECK(query.return_clause, common::InvalidArgumentError,
          Missing("RETURN clause"));
    return BuildQuerySegment(query.reading_clauses, query.return_clause.get(),
                             {});
  }

  SinglePlannerQuery BuildMultiPartQuery(const ast::MultiPartQuery &query) {
    CHECK(query.final_single_part_query, common::InvalidArgumentError,
          Missing("final single query"));

    SinglePlannerQuery root;
    SinglePlannerQuery *current_segment = &root;
    std::unordered_set<std::string> argument_ids;
    for (const auto &part : query.parts) {
      CheckNoUpdatingClauses(part.updating_clauses);
      CHECK(part.with_clause, common::InvalidArgumentError,
            Missing("WITH clause"));
      *current_segment = BuildQuerySegment(
          part.reading_clauses, part.with_clause.get(), argument_ids);
      current_segment = current_segment->Last();
      argument_ids =
          ProjectionOutputSymbols(current_segment->horizon.RequireProjection());
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
    CheckNoUpdatingClauses(query.updating_clauses);
    CHECK(query.return_clause, common::InvalidArgumentError,
          Missing("RETURN clause"));
    return BuildQuerySegment(query.reading_clauses, query.return_clause.get(),
                             argument_ids);
  }

  Projection BuildProjectionClause(
      const ast::ProjectionClause *projection_clause) {
    CHECK(projection_clause != nullptr, common::InvalidArgumentError,
          Missing("projection clause"));
    Projection projection;
    CHECK(projection_clause->body, common::InvalidArgumentError,
          Missing("projection body"));
    projection = BuildProjectionBody(*projection_clause->body);
    if (projection_clause->Is(ast::ASTNodeType::kWith)) {
      const auto *with_clause = ast::CastAst<ast::With>(projection_clause);
      projection.where = with_clause->where.get();
    }
    return projection;
  }

  SinglePlannerQuery BuildQuerySegment(
      const std::vector<std::unique_ptr<ast::ReadingClause>> &reading,
      const ast::ProjectionClause *projection,
      const std::unordered_set<std::string> &argument_ids) {
    CHECK(projection != nullptr, common::InvalidArgumentError,
          Missing("projection clause"));
    SinglePlannerQuery root;
    SinglePlannerQuery *current_segment = &root;
    std::unordered_set<std::string> current_argument_ids = argument_ids;
    QueryGraphBuilder builder(current_argument_ids);
    for (const auto &clause : reading) {
      CHECK(clause != nullptr, common::InvalidArgumentError,
            Missing("reading clause"));
      if (clause->Is(ast::ASTNodeType::kUnwind)) {
        current_segment->query_graph = builder.Release();
        current_segment->horizon = QueryHorizon::ForUnwind(
            BuildUnwindHorizon(*ast::CastAst<ast::Unwind>(clause.get())));
        current_argument_ids =
            QueryGraphAvailableSymbols(current_segment->query_graph);
        current_argument_ids.insert(
            current_segment->horizon.RequireUnwind().alias);
        current_segment->tail = std::make_unique<SinglePlannerQuery>();
        current_segment = current_segment->tail.get();
        builder = QueryGraphBuilder(current_argument_ids);
        continue;
      }
      builder.BuildReadingClause(*clause);
    }

    current_segment->query_graph = builder.Release();
    current_segment->horizon =
        QueryHorizon::ForProjection(BuildProjectionClause(projection));
    return root;
  }

  static Projection BuildProjectionBody(const ast::ProjectionBody &body) {
    CHECK(!body.star, common::InvalidArgumentError,
          Unsupported("projection star before rewrite"));

    Projection projection;
    projection.distinct = body.distinct;

    projection.items.reserve(body.items.size());
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
      projection.items.push_back(std::move(projection_item));
    }

    projection.order_by.reserve(body.order_by.size());
    for (const auto &item : body.order_by) {
      CHECK(item, common::InvalidArgumentError,
            "null sort item is not supported");
      CHECK(item->expression, common::InvalidArgumentError,
            Missing("sort expression"));
      SortItem sort_item;
      sort_item.expression = item->expression.get();
      sort_item.ascending = item->ascending;
      projection.order_by.emplace_back(sort_item);
    }

    projection.skip = body.skip.get();
    projection.limit = body.limit.get();
    return projection;
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
};

}  // namespace

std::unique_ptr<PlannerQuery> CreatePlannerQuery(
    const ast::Statement &statement) {
  PlannerQueryBuilder builder;
  return builder.Build(statement);
}

}  // namespace ir
