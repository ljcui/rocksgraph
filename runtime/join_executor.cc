#include "runtime/join_executor.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "common/exception.h"
#include "runtime/expression_evaluator.h"
#include "runtime/query_row_util.h"
#include "value/value.h"

namespace rg {
namespace {

std::optional<CompositeValueKey> CompositeJoinKey(
    const QueryRow &row, const std::vector<std::string> &join_keys) {
  CompositeValueKey key;
  key.values.reserve(join_keys.size());
  for (const auto &name : join_keys) {
    const Value &value = LookupQueryVariable(row, name);
    if (value.IsNull()) {
      return std::nullopt;
    }
    key.values.push_back(value);
  }
  return key;
}

std::optional<CompositeValueKey> ValueJoinKey(
    const QueryRow &row, const std::vector<ir::ValueHashJoinKey> &join_keys,
    std::size_t child, ExecutionContext context) {
  CompositeValueKey key;
  key.values.reserve(join_keys.size());
  for (const auto &join_key : join_keys) {
    const ast::Expression *expression =
        child == 0 ? join_key.left : join_key.right;
    CHECK(expression != nullptr, common::InternalError,
          "value hash join key expression is null");
    Value value = EvaluateExpression(*expression, row, {}, context);
    if (value.IsNull()) {
      return std::nullopt;
    }
    key.values.push_back(std::move(value));
  }
  return key;
}

bool BuildLeft(const ir::ValueHashJoinPlan &plan) {
  const auto &left_rows = plan.Child(0).EstimatedRows();
  const auto &right_rows = plan.Child(1).EstimatedRows();
  return left_rows.has_value() && right_rows.has_value() &&
         *left_rows < *right_rows;
}

bool PredicatesMatch(const std::vector<const ast::Expression *> &predicates,
                     const QueryRow &row, ExecutionContext context) {
  for (const ast::Expression *predicate : predicates) {
    CHECK(predicate != nullptr, common::InvalidArgumentError,
          "join predicate is null");
    if (!PredicateIsTrue(EvaluateExpression(*predicate, row, {}, context))) {
      return false;
    }
  }
  return true;
}

template <typename Predicate>
QueryRows ExecuteNestedLoopJoin(const QueryRows &left, const QueryRows &right,
                                Predicate predicate) {
  QueryRows out;
  for (const auto &left_row : left) {
    for (const auto &right_row : right) {
      QueryRow merged;
      if (!MergeQueryRows(left_row, right_row, &merged)) {
        continue;
      }
      if (predicate(merged)) {
        out.push_back(std::move(merged));
      }
    }
  }
  return out;
}

}  // namespace

QueryRows JoinExecutor::Execute(const ir::CartesianProductPlan &plan,
                                const QueryRows &left,
                                const QueryRows &right) const {
  (void)plan;
  return ExecuteNestedLoopJoin(left, right,
                               [](const QueryRow &) { return true; });
}

QueryRows JoinExecutor::Execute(const ir::NodeHashJoinPlan &plan,
                                const QueryRows &left,
                                const QueryRows &right) const {
  std::unordered_map<CompositeValueKey, std::vector<const QueryRow *>,
                     ValueHash, ValueEqual>
      buckets;
  for (const auto &row : right) {
    std::optional<CompositeValueKey> key =
        CompositeJoinKey(row, plan.JoinKeys());
    if (key.has_value()) {
      buckets[*key].push_back(&row);
    }
  }

  QueryRows out;
  for (const auto &left_row : left) {
    std::optional<CompositeValueKey> key =
        CompositeJoinKey(left_row, plan.JoinKeys());
    if (!key.has_value()) {
      continue;
    }
    const auto found = buckets.find(*key);
    if (found == buckets.end()) {
      continue;
    }
    for (const QueryRow *right_row : found->second) {
      CHECK(right_row != nullptr, common::InternalError,
            "node hash join row is null");
      QueryRow merged;
      if (MergeQueryRows(left_row, *right_row, &merged)) {
        out.push_back(std::move(merged));
      }
    }
  }
  return out;
}

QueryRows JoinExecutor::Execute(const ir::ValueHashJoinPlan &plan,
                                const QueryRows &left,
                                const QueryRows &right) const {
  const bool build_left = BuildLeft(plan);
  const std::size_t build_child = build_left ? 0U : 1U;
  const std::size_t probe_child = 1U - build_child;
  const QueryRows &build_rows = build_left ? left : right;
  const QueryRows &probe_rows = build_left ? right : left;
  std::unordered_map<CompositeValueKey, std::vector<const QueryRow *>,
                     ValueHash, ValueEqual>
      buckets;
  for (const QueryRow &build_row : build_rows) {
    context_.CheckCancelled();
    std::optional<CompositeValueKey> key =
        ValueJoinKey(build_row, plan.JoinKeys(), build_child, context_);
    if (key.has_value()) {
      buckets[std::move(*key)].push_back(&build_row);
    }
  }

  QueryRows out;
  for (const QueryRow &probe_row : probe_rows) {
    context_.CheckCancelled();
    const std::optional<CompositeValueKey> key =
        ValueJoinKey(probe_row, plan.JoinKeys(), probe_child, context_);
    if (!key.has_value()) {
      continue;
    }
    const auto bucket = buckets.find(*key);
    if (bucket == buckets.end()) {
      continue;
    }
    for (const QueryRow *build_row : bucket->second) {
      context_.CheckCancelled();
      CHECK(build_row != nullptr, common::InternalError,
            "value hash join row is null");
      QueryRow merged;
      const bool merged_rows =
          build_left ? MergeQueryRows(*build_row, probe_row, &merged)
                     : MergeQueryRows(probe_row, *build_row, &merged);
      if (merged_rows && PredicatesMatch(plan.Predicates(), merged, context_)) {
        out.push_back(std::move(merged));
      }
    }
  }
  return out;
}

QueryRows JoinExecutor::Execute(const ir::PredicateJoinPlan &plan,
                                const QueryRows &left,
                                const QueryRows &right) const {
  return ExecuteNestedLoopJoin(left, right, [this, &plan](const QueryRow &row) {
    return PredicatesMatch(plan.Predicates(), row, context_);
  });
}

}  // namespace rg
