#include "planner/planner_query.h"

#include <string>
#include <unordered_map>
#include <utility>

#include "common/exception.h"

namespace planner {
namespace {

class PlannerQueryBuilder {
 public:
  PlannerQuery build(ast::Statement &statement) {
    if (auto regular = dynamic_cast<ast::RegularQuery*>(&statement)) {
      return buildRegularQuery(*regular);
    }
    THROW(common::InvalidArgumentError, "unsupported query type");
  }

 private:
  class QueryGraphBuilder {
   public:
    explicit QueryGraphBuilder(PlannerQueryBuilder &owner)
        : owner_(owner) {}

    void addWhere(const ast::Expression* where) {
      graph_.where.push_back(where);
    }

    void addPattern(const ast::Pattern &pattern) {
      for (const auto &part : pattern.parts) {
        if (part) {
          addPatternPart(*part);
        }
      }
    }

    void addPatternPart(const ast::PatternPart &part) {
      if (!part.variable.empty()) {
        THROW(common::InvalidArgumentError, "named path is not supported");
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
            "anonymous node is not supported");
      graph_.nodes.insert(node.variable);
      return node.variable;
    }

    void addRelationship(const ast::RelationshipPattern &pattern,
                         const std::string &left, const std::string &right) {
      const ast::RelationshipDetail *detail = pattern.detail.get();
      CHECK(detail && !detail->variable.empty(), common::InvalidArgumentError,
            "anonymous relationship is not supported");
      if (pattern.detail && pattern.detail->range) {
        THROW(common::InvalidArgumentError,
              "variable length relationship is not supported");
      }
      QueryGraph::Relationship rel;
      rel.name = detail->variable;
      rel.left_node = left;
      rel.right_node = right;
      if (pattern.left_arrow) {
        rel.direction = QueryGraph::Direction::INCOMING;
      } else if (pattern.right_arrow) {
        rel.direction = QueryGraph::Direction::OUTGOING;
      } else {
        rel.direction = QueryGraph::Direction::BOTH;
      }
      graph_.relationships.push_back(std::move(rel));
    }

    PlannerQueryBuilder &owner_;
    QueryGraph graph_;
  };

  PlannerQuery buildRegularQuery(ast::RegularQuery &query) {
    if (!query.unions.empty()) {
      THROW(common::InvalidArgumentError, "UNION is not supported");
    }
    PlannerQuery pq;
    CHECK(query.single_query, common::InvalidArgumentError,
          "missing single query");
    pq.regular.main = buildSingleQuery(*query.single_query);
    return pq;
  }

  SingleQueryIR buildSingleQuery(ast::SingleQuery &query) {
    if (auto *single = dynamic_cast<ast::SinglePartQuery *>(&query)) {
      SingleQueryIR single_query_ir;
      buildSinglePartQuery(single_query_ir, *single);
      return single_query_ir;
    }
    if (dynamic_cast<ast::MultiPartQuery *>(&query)) {
      THROW(common::InvalidArgumentError, "WITH is not supported");
    }
    THROW(common::InvalidArgumentError, "unsupported single query type");
  }

  void buildSinglePartQuery(SingleQueryIR& ir, ast::SinglePartQuery &query) {
    if (!query.updating_clauses.empty()) {
      THROW(common::InvalidArgumentError, "updating clause is not supported");
    }
    buildQueryPart(ir, query.reading_clauses, query.return_clause.get());
  }

  void buildQueryPart(SingleQueryIR& ir,
      const std::vector<std::unique_ptr<ast::ReadingClause>> &reading,
      const ast::ProjectionClause *projection_clause) {
    QueryGraphBuilder graph_builder(*this);
    for (const auto &clause : reading) {
      if (!clause) {
        continue;
      }
      collectReadingClause(*clause, graph_builder);
    }
    ir.query_graph = graph_builder.release();
    if (projection_clause && projection_clause->body) {
      ir.projection = buildProjection(*projection_clause->body);
    }
  }

  void collectReadingClause(const ast::ReadingClause &clause,
                            QueryGraphBuilder &graph_builder) {
    if (auto *match = dynamic_cast<const ast::Match *>(&clause)) {
      collectMatch(*match, graph_builder);
      return;
    }
    if (dynamic_cast<const ast::Unwind *>(&clause)) {
      THROW(common::InvalidArgumentError, "UNWIND is not supported");
    }
    if (dynamic_cast<const ast::InQueryCall *>(&clause)) {
      THROW(common::InvalidArgumentError, "procedure call is not supported");
    }
    THROW(common::InvalidArgumentError, "unsupported reading clause");
  }

  void collectMatch(const ast::Match &match,
                    QueryGraphBuilder &graph_builder) {
    if (!match.pattern) {
      return;
    }
    if (match.optional_match) {
      THROW(common::InvalidArgumentError, "OPTIONAL MATCH is not supported");
    }
    graph_builder.addPattern(*match.pattern);
    if (match.where) {
      graph_builder.addWhere(match.where.get());
    }
  }

  Projection buildProjection(const ast::ProjectionBody &body) {
    Projection out;
    out.distinct = body.distinct;
    for (const auto &item : body.items) {
      ProjectionItem pi;
      pi.expression = item ? item->expression.get() : nullptr;
      if (item) {
        pi.alias = item->alias;
      }
      out.items.push_back(std::move(pi));
    }
    for (const auto &item : body.order_by) {
      SortItem si;
      si.expression = item ? item->expression.get() : nullptr;
      if (item) {
        si.ascending = item->ascending;
      }
      out.order_by.push_back(std::move(si));
    }
    out.skip = body.skip.get();
    out.limit = body.limit.get();
    return out;
  }

 public:
  PlannerQueryBuilder() = default;
};

}  // namespace

PlannerQuery buildPlannerQuery(ast::Statement &statement) {
  PlannerQueryBuilder builder;
  return builder.build(statement);
}

}  // namespace planner
