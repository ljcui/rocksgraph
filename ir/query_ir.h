#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast_node.h"

namespace ir {

struct QueryGraph {
  enum class Direction { kIncoming, kOutgoing, kBoth };

  struct WherePredicate {
    const ast::Expression *expression = nullptr;
    std::unordered_set<std::string> dependencies;
  };

  struct Relationship {
    std::string name;
    std::string left_node;
    std::string right_node;
    Direction direction = Direction::kBoth;
    std::unordered_set<std::string> types;
  };

  std::unordered_set<std::string> nodes;
  std::vector<Relationship> relationships;
  std::vector<WherePredicate> where;
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
  const ast::Expression *where = nullptr;
  const ast::Expression *skip = nullptr;
  const ast::Expression *limit = nullptr;
};

struct SingleQueryIR {
  QueryGraph query_graph;
  Projection projection;
  std::unique_ptr<SingleQueryIR> tail;

  SingleQueryIR() = default;
  SingleQueryIR(const SingleQueryIR &other);
  SingleQueryIR &operator=(const SingleQueryIR &other);
  SingleQueryIR(SingleQueryIR &&other) noexcept = default;
  SingleQueryIR &operator=(SingleQueryIR &&other) noexcept = default;
  ~SingleQueryIR() = default;

  [[nodiscard]] const SingleQueryIR *Last() const;
  SingleQueryIR *Last();
};

struct UnionColumnMapping {
  std::string output;
  std::string from_main;
  std::string from_branch;
};

struct UnionBranch {
  bool all = false;
  SingleQueryIR query;
  std::vector<UnionColumnMapping> mappings;
};

struct RegularQueryIR {
  SingleQueryIR main;
  std::vector<UnionBranch> unions;
};

struct QueryIR {
  RegularQueryIR regular;
};

QueryIR BuildStatement(const ast::Statement &statement);

}  // namespace ir
