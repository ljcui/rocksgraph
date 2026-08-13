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

std::optional<std::string> CompositeJoinKey(
    const QueryRow &row, const std::vector<std::string> &join_keys) {
  std::string key;
  for (const auto &name : join_keys) {
    const Value &value = LookupQueryVariable(row, name);
    if (value.IsNull()) {
      return std::nullopt;
    }
    const std::string value_key = ValueKey(value);
    key.append(std::to_string(value_key.size()));
    key.push_back(':');
    key.append(value_key);
  }
  return key;
}

bool PredicatesMatch(const std::vector<const ast::Expression *> &predicates,
                     const QueryRow &row) {
  for (const ast::Expression *predicate : predicates) {
    CHECK(predicate != nullptr, common::InvalidArgumentError,
          "join predicate is null");
    if (!PredicateIsTrue(EvaluateExpression(*predicate, row))) {
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
  std::unordered_map<std::string, std::vector<const QueryRow *>> buckets;
  for (const auto &row : right) {
    std::optional<std::string> key = CompositeJoinKey(row, plan.JoinKeys());
    if (key.has_value()) {
      buckets[*key].push_back(&row);
    }
  }

  QueryRows out;
  for (const auto &left_row : left) {
    std::optional<std::string> key =
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
  return ExecuteNestedLoopJoin(left, right, [&plan](const QueryRow &row) {
    return PredicatesMatch(plan.Predicates(), row);
  });
}

QueryRows JoinExecutor::Execute(const ir::PredicateJoinPlan &plan,
                                const QueryRows &left,
                                const QueryRows &right) const {
  return ExecuteNestedLoopJoin(left, right, [&plan](const QueryRow &row) {
    return PredicatesMatch(plan.Predicates(), row);
  });
}

}  // namespace rg
