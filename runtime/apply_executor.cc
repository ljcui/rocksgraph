#include "runtime/apply_executor.h"

#include <utility>

#include "common/exception.h"
#include "runtime/query_row_util.h"
#include "value/value.h"

namespace rg {
namespace {

void CheckExecutor(const ApplyPlanExecutor &execute_plan) {
  CHECK(static_cast<bool>(execute_plan), common::InvalidArgumentError,
        "apply plan executor is empty");
}

QueryRows ExecuteLeft(const ir::LogicalPlan &plan, const QueryRows &input,
                      const ApplyPlanExecutor &execute_plan) {
  CheckExecutor(execute_plan);
  return execute_plan(plan.Child(0), input);
}

QueryRows ExecuteRight(const ir::LogicalPlan &plan, const QueryRow &left,
                       const ApplyPlanExecutor &execute_plan) {
  return execute_plan(plan.Child(1), QueryRows{left});
}

}  // namespace

QueryRows ApplyExecutor::Execute(const ir::ApplyPlan &plan,
                                 const QueryRows &input,
                                 const ApplyPlanExecutor &execute_plan) const {
  QueryRows out;
  for (const auto &left : ExecuteLeft(plan, input, execute_plan)) {
    for (const auto &right : ExecuteRight(plan, left, execute_plan)) {
      QueryRow merged;
      if (MergeQueryRows(left, right, &merged)) {
        out.push_back(std::move(merged));
      }
    }
  }
  return out;
}

QueryRows ApplyExecutor::Execute(const ir::SemiApplyPlan &plan,
                                 const QueryRows &input,
                                 const ApplyPlanExecutor &execute_plan) const {
  QueryRows out;
  for (const auto &left : ExecuteLeft(plan, input, execute_plan)) {
    if (!ExecuteRight(plan, left, execute_plan).empty()) {
      out.push_back(left);
    }
  }
  return out;
}

QueryRows ApplyExecutor::Execute(const ir::AntiSemiApplyPlan &plan,
                                 const QueryRows &input,
                                 const ApplyPlanExecutor &execute_plan) const {
  QueryRows out;
  for (const auto &left : ExecuteLeft(plan, input, execute_plan)) {
    if (ExecuteRight(plan, left, execute_plan).empty()) {
      out.push_back(left);
    }
  }
  return out;
}

QueryRows ApplyExecutor::Execute(const ir::LetSemiApplyPlan &plan,
                                 const QueryRows &input,
                                 const ApplyPlanExecutor &execute_plan) const {
  QueryRows out;
  for (const auto &left : ExecuteLeft(plan, input, execute_plan)) {
    QueryRow result = left;
    result[plan.ValueVariable()] =
        Value(!ExecuteRight(plan, left, execute_plan).empty());
    out.push_back(std::move(result));
  }
  return out;
}

QueryRows ApplyExecutor::Execute(const ir::RollUpApplyPlan &plan,
                                 const QueryRows &input,
                                 const ApplyPlanExecutor &execute_plan) const {
  QueryRows out;
  for (const auto &left : ExecuteLeft(plan, input, execute_plan)) {
    Value::List values;
    for (const auto &right : ExecuteRight(plan, left, execute_plan)) {
      const auto found = right.find(plan.ValueVariable());
      CHECK(found != right.end(), common::InvalidArgumentError,
            "roll-up value variable is not bound: " + plan.ValueVariable());
      values.push_back(found->second);
    }
    QueryRow result = left;
    result[plan.CollectionVariable()] = Value(std::move(values));
    out.push_back(std::move(result));
  }
  return out;
}

QueryRows ApplyExecutor::Execute(const ir::OptionalApplyPlan &plan,
                                 const QueryRows &input,
                                 const ApplyPlanExecutor &execute_plan) const {
  QueryRows out;
  for (const auto &left : ExecuteLeft(plan, input, execute_plan)) {
    QueryRows right_rows = ExecuteRight(plan, left, execute_plan);
    if (right_rows.empty()) {
      QueryRow null_extended = left;
      for (const auto &column : plan.Child(1).OutputColumns()) {
        if (!null_extended.contains(column)) {
          null_extended.emplace(column, Value::Null());
        }
      }
      out.push_back(std::move(null_extended));
      continue;
    }
    for (const auto &right : right_rows) {
      QueryRow merged;
      if (MergeQueryRows(left, right, &merged)) {
        out.push_back(std::move(merged));
      }
    }
  }
  return out;
}

}  // namespace rg
