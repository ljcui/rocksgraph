#include "runtime/row_operator_executor.h"

#include <set>
#include <string>
#include <utility>

#include "common/exception.h"
#include "runtime/aggregation_evaluator.h"
#include "runtime/expression_evaluator.h"
#include "value/value.h"

namespace rg {

QueryRows RowOperatorExecutor::Execute(const ir::ArgumentPlan &plan,
                                       const QueryRows &input) const {
  QueryRows out;
  out.reserve(input.size());
  for (const auto &row : input) {
    QueryRow projected;
    if (plan.OutputColumns().empty()) {
      projected = row;
    } else {
      for (const auto &column : plan.OutputColumns()) {
        const auto found = row.find(column);
        CHECK(found != row.end(), common::InvalidArgumentError,
              "argument variable is not bound: " + column);
        projected.emplace(column, found->second);
      }
    }
    out.push_back(std::move(projected));
  }
  return out;
}

QueryRows RowOperatorExecutor::Execute(const ir::FilterPlan &plan,
                                       QueryRows rows) const {
  QueryRows out;
  for (auto &row : rows) {
    if (PredicateIsTrue(EvaluateExpression(*plan.Predicate(), row,
                                           plan.PrecomputedExpressions()))) {
      out.push_back(std::move(row));
    }
  }
  return out;
}

QueryRows RowOperatorExecutor::Execute(const ir::ProjectionPlan &plan,
                                       const QueryRows &rows) const {
  QueryRows out;
  out.reserve(rows.size());
  for (const auto &row : rows) {
    QueryRow projected;
    for (const auto &item : plan.Items()) {
      if (item.passthrough) {
        const auto found = row.find(item.alias);
        CHECK(found != row.end(), common::InvalidArgumentError,
              "passthrough projection variable is not bound: " + item.alias);
        projected[item.alias] = found->second;
      } else {
        projected[item.alias] = EvaluateLogicalProjectionItem(item, row);
      }
    }
    out.push_back(std::move(projected));
  }
  return out;
}

QueryRows RowOperatorExecutor::Execute(const ir::DistinctPlan &plan,
                                       const QueryRows &rows) const {
  return ProjectDistinctRows(plan.GroupingItems(), rows);
}

QueryRows RowOperatorExecutor::Execute(const ir::AggregationPlan &plan,
                                       const QueryRows &rows) const {
  return AggregateRows(plan.GroupingItems(), plan.AggregationItems(), rows);
}

QueryRows RowOperatorExecutor::Execute(const ir::UnwindPlan &plan,
                                       const QueryRows &rows) const {
  QueryRows out;
  for (const auto &row : rows) {
    Value list = EvaluateExpression(*plan.Expression(), row);
    if (!list.IsList()) {
      continue;
    }
    for (const auto &item : list.AsList()) {
      QueryRow next = row;
      next[plan.Alias()] = item;
      out.push_back(std::move(next));
    }
  }
  return out;
}

QueryRows RowOperatorExecutor::Execute(const ir::UnionPlan &plan,
                                       const QueryRows &left_rows,
                                       const QueryRows &right_rows) const {
  QueryRows out;
  std::set<std::string> seen;

  auto append_rows = [&](const QueryRows &rows, bool left_side) {
    for (const auto &row : rows) {
      QueryRow mapped;
      std::string key;
      for (const auto &mapping : plan.Mappings()) {
        const std::string &source =
            left_side ? mapping.lhs_variable : mapping.rhs_variable;
        const auto found = row.find(source);
        CHECK(found != row.end(), common::InvalidArgumentError,
              "UNION source variable is not bound: " + source);
        mapped[mapping.output_variable] = found->second;
        if (!plan.All()) {
          key += mapping.output_variable;
          key += '=';
          key += ValueKey(found->second);
          key += '\n';
        }
      }
      if (plan.All() || seen.insert(std::move(key)).second) {
        out.push_back(std::move(mapped));
      }
    }
  };

  append_rows(left_rows, true);
  append_rows(right_rows, false);
  return out;
}

}  // namespace rg
