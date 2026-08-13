#include "runtime/result_set_executor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ast/ast_const_walker.h"
#include "ast/ast_equal.h"
#include "ast/expression_dependency.h"
#include "common/exception.h"
#include "runtime/expression_evaluator.h"
#include "value/value.h"

namespace rg {
namespace {

enum class ValueOrder {
  kLess,
  kEquivalent,
  kGreater,
};

bool ContainsExpression(const ast::Expression &haystack,
                        const ast::Expression &needle) {
  class Finder final : public ast::ASTConstWalker {
   public:
    explicit Finder(const ast::Expression &needle) : needle_(needle) {}

    [[nodiscard]] bool Found() const noexcept { return found_; }

   protected:
    void Visit(const ast::ExistentialSubquery &node) override {
      found_ = found_ || ast::ASTEqual::Equal(&node, &needle_);
      ast::ASTConstWalker::Visit(node);
    }

    void Visit(const ast::PatternComprehension &node) override {
      found_ = found_ || ast::ASTEqual::Equal(&node, &needle_);
      ast::ASTConstWalker::Visit(node);
    }

   private:
    const ast::Expression &needle_;
    bool found_ = false;
  };

  if (ast::ASTEqual::Equal(&haystack, &needle)) {
    return true;
  }
  Finder finder(needle);
  haystack.Accept(finder);
  return finder.Found();
}

bool RequiresInputRow(
    const ast::Expression &expression,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  if (!ast::CollectExpressionDependencies(expression).empty()) {
    return true;
  }
  return std::any_of(
      precomputed.begin(), precomputed.end(),
      [&expression](const ir::LogicalPrecomputedExpression &entry) {
        return entry.expression != nullptr &&
               ContainsExpression(expression, *entry.expression);
      });
}

ValueOrder CompareValues(const Value &left, const Value &right) {
  if (ValuesEqual(left, right)) {
    return ValueOrder::kEquivalent;
  }

  const bool left_less = ValueLess(left, right);
  const bool right_less = ValueLess(right, left);
  if (left_less != right_less) {
    return left_less ? ValueOrder::kLess : ValueOrder::kGreater;
  }

  const std::string left_key = ValueKey(left);
  const std::string right_key = ValueKey(right);
  if (left_key < right_key) {
    return ValueOrder::kLess;
  }
  if (right_key < left_key) {
    return ValueOrder::kGreater;
  }
  return ValueOrder::kEquivalent;
}

ValueOrder CompareSortItem(const ir::LogicalSortItem &item,
                           const QueryRow &left, const QueryRow &right) {
  const Value lhs = EvaluateLogicalSortItem(item, left);
  const Value rhs = EvaluateLogicalSortItem(item, right);
  const ValueOrder order = CompareValues(lhs, rhs);
  if (item.direction == ir::LogicalOrderDirection::kAscending) {
    return order;
  }
  if (order == ValueOrder::kLess) {
    return ValueOrder::kGreater;
  }
  if (order == ValueOrder::kGreater) {
    return ValueOrder::kLess;
  }
  return ValueOrder::kEquivalent;
}

std::optional<std::int64_t> EvaluatePaginationCount(
    const ast::Expression *expression,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    const QueryRows &rows, const std::string &operator_name) {
  CHECK(expression != nullptr, common::InvalidArgumentError,
        operator_name + " expression is null");
  if (rows.empty() && RequiresInputRow(*expression, precomputed)) {
    return std::nullopt;
  }

  const QueryRow empty_row;
  const QueryRow &row = rows.empty() ? empty_row : rows.front();
  const Value value = EvaluateExpression(*expression, row, precomputed);
  CHECK(value.IsInteger(), common::InvalidArgumentError,
        operator_name + " requires a non-negative integer");
  CHECK(value.AsInteger() >= 0, common::InvalidArgumentError,
        operator_name + " requires a non-negative integer");
  return value.AsInteger();
}

}  // namespace

QueryRows ResultSetExecutor::Execute(const ir::SortPlan &plan,
                                     QueryRows rows) const {
  std::stable_sort(rows.begin(), rows.end(),
                   [&plan](const QueryRow &left, const QueryRow &right) {
                     for (const auto &item : plan.Items()) {
                       const ValueOrder order =
                           CompareSortItem(item, left, right);
                       if (order == ValueOrder::kLess) {
                         return true;
                       }
                       if (order == ValueOrder::kGreater) {
                         return false;
                       }
                     }
                     return false;
                   });
  return rows;
}

QueryRows ResultSetExecutor::Execute(const ir::SkipPlan &plan,
                                     QueryRows rows) const {
  const std::optional<std::int64_t> count = EvaluatePaginationCount(
      plan.Skip(), plan.PrecomputedExpressions(), rows, "SKIP");
  if (!count.has_value() || static_cast<std::uint64_t>(*count) >= rows.size()) {
    return {};
  }
  return {rows.begin() + static_cast<std::ptrdiff_t>(*count), rows.end()};
}

QueryRows ResultSetExecutor::Execute(const ir::LimitPlan &plan,
                                     QueryRows rows) const {
  const std::optional<std::int64_t> count = EvaluatePaginationCount(
      plan.Limit(), plan.PrecomputedExpressions(), rows, "LIMIT");
  if (count.has_value() && static_cast<std::uint64_t>(*count) < rows.size()) {
    rows.resize(static_cast<std::size_t>(*count));
  }
  return rows;
}

}  // namespace rg
