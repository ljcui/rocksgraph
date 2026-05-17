#pragma once

#include <memory>
#include <string>
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

struct UnwindHorizon {
  const ast::Expression *expression = nullptr;
  std::string alias;
};

enum class QueryHorizonKind { kProjection, kUnwind };

struct QueryHorizon {
  QueryHorizonKind kind = QueryHorizonKind::kProjection;
  Projection projection;
  UnwindHorizon unwind;

  static QueryHorizon ForProjection(Projection projection);
  static QueryHorizon ForUnwind(UnwindHorizon unwind);

  [[nodiscard]] const Projection &RequireProjection() const;
  Projection &RequireProjection();

  [[nodiscard]] const UnwindHorizon &RequireUnwind() const;
  UnwindHorizon &RequireUnwind();
};

struct SinglePlannerQuery;
struct UnionPlannerQuery;

enum class PlannerQueryKind { kSingle, kUnion };

class PlannerQuery {
 public:
  PlannerQuery() = default;
  PlannerQuery(const PlannerQuery &) = delete;
  PlannerQuery &operator=(const PlannerQuery &) = delete;
  PlannerQuery(PlannerQuery &&) noexcept = default;
  PlannerQuery &operator=(PlannerQuery &&) noexcept = default;
  virtual ~PlannerQuery() = default;

  [[nodiscard]] virtual PlannerQueryKind Kind() const = 0;

  [[nodiscard]] const SinglePlannerQuery &RequireSingle() const;
  SinglePlannerQuery &RequireSingle();

  [[nodiscard]] const UnionPlannerQuery &RequireUnion() const;
  UnionPlannerQuery &RequireUnion();
};

struct SinglePlannerQuery final : public PlannerQuery {
  QueryGraph query_graph;
  QueryHorizon horizon;
  std::unique_ptr<SinglePlannerQuery> tail;

  SinglePlannerQuery() = default;

  [[nodiscard]] PlannerQueryKind Kind() const final {
    return PlannerQueryKind::kSingle;
  }

  [[nodiscard]] const SinglePlannerQuery *Last() const;
  SinglePlannerQuery *Last();
};

struct UnionPlannerQuery final : public PlannerQuery {
  std::unique_ptr<PlannerQuery> lhs;
  SinglePlannerQuery rhs;
  bool all = false;

  UnionPlannerQuery() = default;

  [[nodiscard]] PlannerQueryKind Kind() const final {
    return PlannerQueryKind::kUnion;
  }
};

std::unique_ptr<PlannerQuery> MakeSinglePlannerQuery(SinglePlannerQuery query);
std::unique_ptr<PlannerQuery> MakeUnionPlannerQuery(
    std::unique_ptr<PlannerQuery> lhs, SinglePlannerQuery rhs, bool all);

std::unique_ptr<PlannerQuery> BuildStatement(const ast::Statement &statement);

}  // namespace ir
