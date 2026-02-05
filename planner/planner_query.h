#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast_node.h"

namespace planner {

struct QueryGraph {
  enum  class Direction {
    INCOMING,
    OUTGOING,
    BOTH
  };
  struct Relationship {
    std::string name;
    std::string left_node;
    std::string right_node;
    Direction direction;
    std::vector<std::string> types;
  };
  std::unordered_set<std::string> nodes;
  std::vector<Relationship> relationships;
  std::vector<const ast::Expression *> where;
};

struct ProjectionItem {
  const ast::Expression *expression = nullptr;
  std::string alias;
};

struct SortItem {
  const ast::Expression *expression = nullptr;
  bool ascending = true;
};

struct Projection {
  bool distinct = false;
  std::vector<ProjectionItem> items;
  std::vector<SortItem> order_by;
  const ast::Expression *skip = nullptr;
  const ast::Expression *limit = nullptr;
};

struct SingleQueryIR {
  QueryGraph query_graph;
  Projection projection;
};

struct UnionBranch {
  bool all = false;
  SingleQueryIR query;
};

struct RegularQueryIR {
  SingleQueryIR main;
  std::vector<UnionBranch> unions;
};

struct PlannerQuery {
  RegularQueryIR regular;
};

PlannerQuery buildPlannerQuery(ast::Statement &statement);

}  // namespace planner
