#include "ir/query_ir.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_const_walker.h"
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

const ast::AndExpression *RequireConjunctiveWhere(
    const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr ||
      !unwrapped->Is(ast::ASTNodeType::kAndExpression)) {
    THROW(common::InvalidArgumentError,
          Unsupported("WHERE without AND expression"));
  }
  return ast::CastAst<ast::AndExpression>(unwrapped);
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

class VariableDependencyCollector : public ast::ASTConstWalker {
 public:
  explicit VariableDependencyCollector(
      std::unordered_set<std::string> *dependencies)
      : dependencies_(dependencies) {
    CHECK(dependencies_ != nullptr, common::InternalError,
          "dependencies output is null");
  }

  void Collect(const ast::Expression &expression) { expression.Accept(*this); }

 protected:
  void Visit(const ast::Variable &node) override {
    CHECK(!node.name.empty(), common::InvalidArgumentError,
          "variable dependency name is empty");
    dependencies_->emplace(node.name);
  }

 private:
  std::unordered_set<std::string> *dependencies_;
};

std::unordered_set<std::string> CollectDependencies(
    const ast::Expression &expression) {
  std::unordered_set<std::string> dependencies;
  VariableDependencyCollector collector(&dependencies);
  collector.Collect(expression);
  return dependencies;
}

std::unique_ptr<SingleQueryIR> CloneTail(
    const std::unique_ptr<SingleQueryIR> &tail) {
  if (!tail) {
    return nullptr;
  }
  return std::make_unique<SingleQueryIR>(*tail);
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

void AppendUnique(std::vector<std::string> &to,
                  const std::vector<std::string> &from) {
  for (const auto &value : from) {
    if (std::find(to.begin(), to.end(), value) == to.end()) {
      to.push_back(value);
    }
  }
}

std::string ResolveProjectionColumnName(const ProjectionItem &item) {
  if (!item.alias.empty()) {
    return item.alias;
  }
  if (item.expression != nullptr &&
      item.expression->Is(ast::ASTNodeType::kVariable)) {
    const auto *variable = ast::CastAst<ast::Variable>(item.expression);
    return variable->name;
  }
  return {};
}

std::vector<std::string> CollectTerminalColumns(const SingleQueryIR &query) {
  const SingleQueryIR *last = LastQueryPart(&query);
  std::vector<std::string> columns;
  columns.reserve(last->projection.items.size());
  for (const auto &item : last->projection.items) {
    const std::string column = ResolveProjectionColumnName(item);
    CHECK(!column.empty(), common::InvalidArgumentError,
          "projection item without alias is not supported");
    columns.push_back(column);
  }
  return columns;
}

}  // namespace

class QueryGraphBuilder {
 public:
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
    AddPattern(*match.pattern);
    if (match.where) {
      AddWhere(match.where.get());
    }
  }

  void AddWhere(const ast::Expression *where) {
    CHECK(where != nullptr, common::InvalidArgumentError,
          "WHERE predicate is null");
    CHECK(where->Is(ast::ASTNodeType::kAndExpression),
          common::InvalidArgumentError, "WHERE predicate is not AND");
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

      QueryGraph::WherePredicate where_predicate;
      where_predicate.expression = predicate;
      where_predicate.dependencies = CollectDependencies(*predicate);
      graph_.where.push_back(std::move(where_predicate));
    }
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

  void AddPatternPart(const ast::PatternPart &part) {
    if (!part.variable.empty()) {
      THROW(common::InvalidArgumentError, Unsupported("named path"));
    }
    CHECK(part.element != nullptr, common::InvalidArgumentError,
          Missing("pattern element"));
    AddPatternElement(*part.element);
  }

  QueryGraph Release() { return std::move(graph_); }

 private:
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
    graph_.nodes.insert(node.variable);
    return node.variable;
  }

  void AddRelationship(const ast::RelationshipPattern &pattern,
                       const std::string &left, const std::string &right) {
    const ast::RelationshipDetail *detail = pattern.detail.get();
    CHECK(detail && !detail->variable.empty(), common::InvalidArgumentError,
          Unsupported("anonymous relationship"));
    CHECK(!detail->range, common::InvalidArgumentError,
          Unsupported("variable length relationship"));
    CHECK(detail->types.empty(), common::InvalidArgumentError,
          Unsupported("relationship with labels"));
    CHECK(!detail->properties, common::InvalidArgumentError,
          Unsupported("relationship with properties"));

    QueryGraph::Relationship relationship;
    relationship.name = detail->variable;
    relationship.left_node = left;
    relationship.right_node = right;
    relationship.types = {detail->types.begin(), detail->types.end()};
    if (pattern.left_arrow) {
      relationship.direction = QueryGraph::Direction::kIncoming;
    } else if (pattern.right_arrow) {
      relationship.direction = QueryGraph::Direction::kOutgoing;
    } else {
      relationship.direction = QueryGraph::Direction::kBoth;
    }
    graph_.relationships.push_back(std::move(relationship));
  }

  QueryGraph graph_;
  std::unordered_set<std::string> where_keys_;
};

SingleQueryIR::SingleQueryIR(const SingleQueryIR &other)
    : query_graph(other.query_graph),
      projection(other.projection),
      tail(CloneTail(other.tail)) {}

SingleQueryIR &SingleQueryIR::operator=(const SingleQueryIR &other) {
  if (this == &other) {
    return *this;
  }
  query_graph = other.query_graph;
  projection = other.projection;
  tail = CloneTail(other.tail);
  return *this;
}

const SingleQueryIR *SingleQueryIR::Last() const { return LastQueryPart(this); }

SingleQueryIR *SingleQueryIR::Last() { return LastQueryPart(this); }

namespace {

void CheckNoUpdatingClauses(
    const std::vector<std::unique_ptr<ast::UpdatingClause>> &updating_clauses) {
  if (!updating_clauses.empty()) {
    THROW(common::InvalidArgumentError, Unsupported("updating clause"));
  }
}

std::vector<UnionColumnMapping> BuildUnionColumnMappings(
    const SingleQueryIR &main_query, const SingleQueryIR &branch_query) {
  const auto main_columns = CollectTerminalColumns(main_query);
  const auto branch_columns = CollectTerminalColumns(branch_query);
  CHECK(main_columns.size() == branch_columns.size(),
        common::InvalidArgumentError,
        "UNION branches must return the same number of columns");

  std::vector<UnionColumnMapping> mappings;
  mappings.reserve(main_columns.size());
  for (size_t index = 0; index < main_columns.size(); ++index) {
    UnionColumnMapping mapping;
    mapping.output = main_columns[index];
    mapping.from_main = main_columns[index];
    mapping.from_branch = branch_columns[index];
    mappings.push_back(std::move(mapping));
  }
  return mappings;
}

class QueryIRBuilder {
 public:
  QueryIR Build(const ast::Statement &statement) {
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
  QueryIR BuildRegularQuery(const ast::RegularQuery &query) {
    CHECK(query.single_query, common::InvalidArgumentError,
          Missing("single query"));

    QueryIR query_ir;
    query_ir.regular.main = BuildSingleQuery(*query.single_query);

    query_ir.regular.unions.reserve(query.unions.size());
    for (const auto &part : query.unions) {
      CHECK(part && part->query, common::InvalidArgumentError,
            Missing("UNION branch query"));

      UnionBranch branch;
      branch.all = part->all;
      branch.query = BuildSingleQuery(*part->query);
      branch.mappings =
          BuildUnionColumnMappings(query_ir.regular.main, branch.query);
      query_ir.regular.unions.push_back(std::move(branch));
    }

    return query_ir;
  }

  SingleQueryIR BuildSingleQuery(const ast::SingleQuery &query) {
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

  SingleQueryIR BuildSinglePartQuery(const ast::SinglePartQuery &query) {
    CheckNoUpdatingClauses(query.updating_clauses);
    CHECK(query.return_clause, common::InvalidArgumentError,
          Missing("RETURN clause"));
    return BuildQuerySegment(query.reading_clauses, query.return_clause.get());
  }

  SingleQueryIR BuildMultiPartQuery(const ast::MultiPartQuery &query) {
    CHECK(query.final_single_part_query, common::InvalidArgumentError,
          Missing("final single query"));

    SingleQueryIR root;
    SingleQueryIR *current_segment = &root;
    for (const auto &part : query.parts) {
      CheckNoUpdatingClauses(part.updating_clauses);
      CHECK(part.with_clause, common::InvalidArgumentError,
            Missing("WITH clause"));
      *current_segment =
          BuildQuerySegment(part.reading_clauses, part.with_clause.get());
      current_segment->tail = std::make_unique<SingleQueryIR>();
      current_segment = current_segment->tail.get();
    }

    *current_segment = BuildSinglePartQuery(*query.final_single_part_query);
    return root;
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

  SingleQueryIR BuildQuerySegment(
      const std::vector<std::unique_ptr<ast::ReadingClause>> &reading,
      const ast::ProjectionClause *projection) {
    CHECK(projection != nullptr, common::InvalidArgumentError,
          Missing("projection clause"));
    SingleQueryIR ir;
    QueryGraphBuilder builder;
    for (const auto &clause : reading) {
      CHECK(clause != nullptr, common::InvalidArgumentError,
            Missing("reading clause"));
      builder.BuildReadingClause(*clause);
    }

    ir.query_graph = builder.Release();
    ir.projection = BuildProjectionClause(projection);
    return ir;
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
};

}  // namespace

QueryIR BuildStatement(const ast::Statement &statement) {
  QueryIRBuilder builder;
  return builder.Build(statement);
}

}  // namespace ir
