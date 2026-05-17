#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast_node.h"

namespace ir {

using LogicalVariable = std::string;

enum class Direction { kIncoming, kOutgoing, kBoth };

struct PatternLength {
  bool variable = false;
  int fixed = 1;
  std::optional<int> min;
  std::optional<int> max;
};

struct PatternRelationship {
  std::string variable;
  std::string left_node;
  std::string right_node;
  Direction direction = Direction::kBoth;
  std::vector<std::string> types;
  PatternLength length;
};

enum class PredicateKind {
  kGenericExpression,
  kNodeLabel,
  kRelationshipType,
  kPropertyEquality,
  kPropertyComparison,
  kExistsSubquery,
};

struct Predicate {
  const ast::Expression *expression = nullptr;
  std::unordered_set<std::string> dependencies;
  PredicateKind kind = PredicateKind::kGenericExpression;
  std::string variable;
  std::string property_key;
  std::vector<std::string> labels;
  std::vector<std::string> relationship_types;
  std::string comparison_op;
};

struct Selections {
  std::vector<Predicate> predicates;

  [[nodiscard]] bool empty() const { return predicates.empty(); }
  [[nodiscard]] std::size_t size() const { return predicates.size(); }
};

struct Hint {};
struct MutatingPattern {};

struct QueryGraph {
  std::unordered_set<LogicalVariable> pattern_nodes;
  std::vector<PatternRelationship> pattern_relationships;
  std::unordered_set<LogicalVariable> argument_ids;
  Selections selections;
  std::vector<QueryGraph> optional_matches;
  std::vector<Hint> hints;
  std::vector<MutatingPattern> mutating_patterns;
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

std::unique_ptr<PlannerQuery> CreatePlannerQuery(
    const ast::Statement &statement);

}  // namespace ir
