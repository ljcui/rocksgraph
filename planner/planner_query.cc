#include "planner/planner_query.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/exception.h"

namespace planner {

namespace {

std::string makeUnsupportedError(std::string_view feature) {
  return std::string(feature) + " is not supported";
}

std::string makeMissingError(std::string_view subject) {
  return "missing " + std::string(subject);
}

std::unique_ptr<SingleQueryIR> cloneTail(
    const std::unique_ptr<SingleQueryIR> &tail) {
  if (!tail) {
    return nullptr;
  }
  return std::make_unique<SingleQueryIR>(*tail);
}

SingleQueryIR *lastQueryPart(SingleQueryIR *query) {
  SingleQueryIR *current = query;
  while (current->tail) {
    current = current->tail.get();
  }
  return current;
}

const SingleQueryIR *lastQueryPart(const SingleQueryIR *query) {
  const SingleQueryIR *current = query;
  while (current->tail) {
    current = current->tail.get();
  }
  return current;
}

void appendUnique(std::vector<std::string> &to,
                  const std::vector<std::string> &from) {
  for (const auto &value : from) {
    if (std::find(to.begin(), to.end(), value) == to.end()) {
      to.push_back(value);
    }
  }
}

std::string resolveProjectionColumnName(const ProjectionItem &item) {
  if (!item.alias.empty()) {
    return item.alias;
  }
  auto *variable = dynamic_cast<const ast::Variable *>(item.expression);
  if (variable) {
    return variable->name;
  }
  return {};
}

std::vector<std::string> collectTerminalColumns(const SingleQueryIR &query) {
  const SingleQueryIR *last = lastQueryPart(&query);
  std::vector<std::string> columns;
  columns.reserve(last->projection.items.size());
  for (const auto &item : last->projection.items) {
    const std::string column = resolveProjectionColumnName(item);
    CHECK(!column.empty(), common::InvalidArgumentError,
          "projection item without alias is not supported");
    columns.push_back(column);
  }
  return columns;
}

}  // namespace

SingleQueryIR::SingleQueryIR(const SingleQueryIR &other)
    : query_graph(other.query_graph),
      projection(other.projection),
      tail(cloneTail(other.tail)) {}

SingleQueryIR &SingleQueryIR::operator=(const SingleQueryIR &other) {
  if (this == &other) {
    return *this;
  }
  query_graph = other.query_graph;
  projection = other.projection;
  tail = cloneTail(other.tail);
  return *this;
}

const SingleQueryIR *SingleQueryIR::last() const { return lastQueryPart(this); }

SingleQueryIR *SingleQueryIR::last() { return lastQueryPart(this); }

namespace {

void checkNoUpdatingClauses(
    const std::vector<std::unique_ptr<ast::UpdatingClause>> &updating_clauses) {
  if (!updating_clauses.empty()) {
    THROW(common::InvalidArgumentError,
          makeUnsupportedError("updating clause"));
  }
}

std::vector<UnionColumnMapping> buildUnionColumnMappings(
    const SingleQueryIR &main_query, const SingleQueryIR &branch_query) {
  const auto main_columns = collectTerminalColumns(main_query);
  const auto branch_columns = collectTerminalColumns(branch_query);
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

SingleQueryIR &appendEmptyTailPart(SingleQueryIR &current) {
  current.tail = std::make_unique<SingleQueryIR>();
  return *current.tail;
}

class PlannerQueryBuilder {
 public:
  PlannerQuery build(ast::Statement &statement) {
    auto *regular = dynamic_cast<ast::RegularQuery *>(&statement);
    if (regular) {
      return buildRegularQuery(*regular);
    }
    THROW(common::InvalidArgumentError, makeUnsupportedError("query type"));
  }

 private:
  class QueryGraphBuilder {
   public:
    void addWhere(const ast::Expression *where) { graph_.where.push_back(where); }

    void addPattern(const ast::Pattern &pattern) {
      for (const auto &part : pattern.parts) {
        if (part) {
          addPatternPart(*part);
        }
      }
    }

    void addPatternPart(const ast::PatternPart &part) {
      if (!part.variable.empty()) {
        THROW(common::InvalidArgumentError,
              makeUnsupportedError("named path"));
      }
      if (part.element) {
        addPatternElement(*part.element);
      }
    }

    QueryGraph release() { return std::move(graph_); }

   private:
    void addPatternElement(const ast::PatternElement &element) {
      if (!element.node_pattern) {
        return;
      }
      std::string left = addNode(*element.node_pattern);
      for (const auto &link : element.chain) {
        if (!link.second) {
          continue;
        }
        std::string right = addNode(*link.second);
        if (link.first) {
          addRelationship(*link.first, left, right);
        }
        left = right;
      }
    }

    std::string addNode(const ast::NodePattern &node) {
      CHECK(!node.variable.empty(), common::InvalidArgumentError,
            makeUnsupportedError("anonymous node"));
      graph_.nodes.insert(node.variable);
      QueryGraph::Node &node_info = graph_.node_info[node.variable];
      appendUnique(node_info.labels, node.labels);
      if (node.properties) {
        CHECK(node_info.properties == nullptr ||
                  node_info.properties == node.properties.get(),
              common::InvalidArgumentError,
              "conflicting properties for the same node variable");
        node_info.properties = node.properties.get();
      }
      return node.variable;
    }

    void addRelationship(const ast::RelationshipPattern &pattern,
                         const std::string &left,
                         const std::string &right) {
      const ast::RelationshipDetail *detail = pattern.detail.get();
      CHECK(detail && !detail->variable.empty(), common::InvalidArgumentError,
            makeUnsupportedError("anonymous relationship"));
      if (detail->range) {
        THROW(common::InvalidArgumentError,
              makeUnsupportedError("variable length relationship"));
      }

      QueryGraph::Relationship relationship;
      relationship.name = detail->variable;
      relationship.left_node = left;
      relationship.right_node = right;
      relationship.types = detail->types;
      relationship.properties = detail->properties.get();
      if (pattern.left_arrow) {
        relationship.direction = QueryGraph::Direction::INCOMING;
      } else if (pattern.right_arrow) {
        relationship.direction = QueryGraph::Direction::OUTGOING;
      } else {
        relationship.direction = QueryGraph::Direction::BOTH;
      }
      graph_.relationships.push_back(std::move(relationship));
    }

    QueryGraph graph_;
  };

  PlannerQuery buildRegularQuery(ast::RegularQuery &query) {
    CHECK(query.single_query, common::InvalidArgumentError,
          makeMissingError("single query"));

    PlannerQuery planner_query;
    planner_query.regular.main = buildSingleQuery(*query.single_query);

    planner_query.regular.unions.reserve(query.unions.size());
    for (const auto &part : query.unions) {
      CHECK(part && part->query, common::InvalidArgumentError,
            makeMissingError("UNION branch query"));

      UnionBranch branch;
      branch.all = part->all;
      branch.query = buildSingleQuery(*part->query);
      branch.mappings =
          buildUnionColumnMappings(planner_query.regular.main, branch.query);
      planner_query.regular.unions.push_back(std::move(branch));
    }

    return planner_query;
  }

  SingleQueryIR buildSingleQuery(ast::SingleQuery &query) {
    if (auto *single_part = dynamic_cast<ast::SinglePartQuery *>(&query)) {
      return buildSinglePartQuery(*single_part);
    }

    if (auto *multi_part = dynamic_cast<ast::MultiPartQuery *>(&query)) {
      return buildMultiPartQuery(*multi_part);
    }

    THROW(common::InvalidArgumentError,
          makeUnsupportedError("single query type"));
  }

  SingleQueryIR buildSinglePartQuery(ast::SinglePartQuery &query) {
    checkNoUpdatingClauses(query.updating_clauses);

    SingleQueryIR single_query_ir;
    populateQuerySegment(single_query_ir, query.reading_clauses,
                         query.return_clause.get());
    return single_query_ir;
  }

  SingleQueryIR buildMultiPartQuery(ast::MultiPartQuery &query) {
    CHECK(query.final_single_part_query, common::InvalidArgumentError,
          makeMissingError("final single query"));

    SingleQueryIR root;
    SingleQueryIR *current_segment = &root;

    for (auto &part : query.parts) {
      checkNoUpdatingClauses(part.updating_clauses);
      CHECK(part.with_clause, common::InvalidArgumentError,
            makeMissingError("WITH clause"));

      populateQuerySegment(*current_segment, part.reading_clauses,
                           part.with_clause.get());
      current_segment = &appendEmptyTailPart(*current_segment);
    }

    *current_segment = buildSinglePartQuery(*query.final_single_part_query);
    return root;
  }

  Projection buildProjectionFromClause(
      const ast::ProjectionClause *projection_clause) {
    Projection projection;
    if (!projection_clause) {
      return projection;
    }
    if (projection_clause->body) {
      projection = buildProjection(*projection_clause->body);
    }
    if (auto *with_clause = dynamic_cast<const ast::With *>(projection_clause)) {
      projection.where = with_clause->where.get();
    }
    return projection;
  }

  void populateQuerySegment(
      SingleQueryIR &ir,
      const std::vector<std::unique_ptr<ast::ReadingClause>> &reading,
      const ast::ProjectionClause *projection_clause) {
    QueryGraphBuilder graph_builder;
    for (const auto &clause : reading) {
      if (!clause) {
        continue;
      }
      populateQueryGraphFromReadingClause(*clause, graph_builder);
    }

    ir.query_graph = graph_builder.release();
    ir.projection = buildProjectionFromClause(projection_clause);
  }

  void populateQueryGraphFromReadingClause(
      const ast::ReadingClause &clause, QueryGraphBuilder &graph_builder) {
    if (auto *match = dynamic_cast<const ast::Match *>(&clause)) {
      populateQueryGraphFromMatch(*match, graph_builder);
      return;
    }
    if (dynamic_cast<const ast::Unwind *>(&clause)) {
      THROW(common::InvalidArgumentError, makeUnsupportedError("UNWIND"));
    }
    if (dynamic_cast<const ast::InQueryCall *>(&clause)) {
      THROW(common::InvalidArgumentError,
            makeUnsupportedError("procedure call"));
    }
    THROW(common::InvalidArgumentError,
          makeUnsupportedError("reading clause"));
  }

  void populateQueryGraphFromMatch(const ast::Match &match,
                                   QueryGraphBuilder &graph_builder) {
    if (!match.pattern) {
      return;
    }
    if (match.optional_match) {
      THROW(common::InvalidArgumentError,
            makeUnsupportedError("OPTIONAL MATCH"));
    }
    graph_builder.addPattern(*match.pattern);
    if (match.where) {
      graph_builder.addWhere(match.where.get());
    }
  }

  Projection buildProjection(const ast::ProjectionBody &body) {
    CHECK(!body.star, common::InvalidArgumentError,
          makeUnsupportedError("projection star before rewrite"));

    Projection projection;
    projection.distinct = body.distinct;

    projection.items.reserve(body.items.size());
    for (const auto &item : body.items) {
      CHECK(item, common::InvalidArgumentError,
            "null projection item is not supported");
      ProjectionItem projection_item;
      projection_item.expression = item->expression.get();
      projection_item.alias = item->alias;
      projection.items.push_back(std::move(projection_item));
    }

    projection.order_by.reserve(body.order_by.size());
    for (const auto &item : body.order_by) {
      CHECK(item, common::InvalidArgumentError,
            "null sort item is not supported");
      SortItem sort_item;
      sort_item.expression = item->expression.get();
      sort_item.ascending = item->ascending;
      projection.order_by.push_back(std::move(sort_item));
    }

    projection.skip = body.skip.get();
    projection.limit = body.limit.get();
    return projection;
  }
};

}  // namespace

PlannerQuery buildPlannerQuery(ast::Statement &statement) {
  PlannerQueryBuilder builder;
  return builder.build(statement);
}

}  // namespace planner
