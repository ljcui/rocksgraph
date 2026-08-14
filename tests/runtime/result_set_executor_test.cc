#include "runtime/result_set_executor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "common/exception.h"
#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "value/value.h"

namespace {

std::unique_ptr<ir::ArgumentPlan> Arguments(std::vector<std::string> columns) {
  return std::make_unique<ir::ArgumentPlan>(std::move(columns));
}

std::unique_ptr<ast::Variable> Variable(std::string name) {
  auto variable = std::make_unique<ast::Variable>();
  variable->name = std::move(name);
  return variable;
}

std::unique_ptr<ast::IntegerLiteral> Integer(std::int64_t value) {
  auto literal = std::make_unique<ast::IntegerLiteral>();
  literal->value = value;
  return literal;
}

std::unique_ptr<ast::ExistentialSubquery> ExistsPattern() {
  auto node = std::make_unique<ast::NodePattern>();
  auto element = std::make_unique<ast::PatternElement>();
  element->node_pattern = std::move(node);
  auto part = std::make_unique<ast::PatternPart>();
  part->element = std::move(element);
  auto pattern = std::make_unique<ast::Pattern>();
  pattern->parts.push_back(std::move(part));
  auto exists = std::make_unique<ast::ExistentialSubquery>();
  exists->pattern = std::move(pattern);
  return exists;
}

std::vector<std::string> ColumnStrings(const rg::QueryRows &rows,
                                       const std::string &column) {
  std::vector<std::string> values;
  values.reserve(rows.size());
  for (const auto &row : rows) {
    values.push_back(row.at(column).AsString());
  }
  return values;
}

ir::LogicalSortItem SortItem(const ast::Expression *expression,
                             ir::LogicalOrderDirection direction =
                                 ir::LogicalOrderDirection::kAscending) {
  return ir::LogicalSortItem{.expression = expression, .direction = direction};
}

}  // namespace

TEST(ResultSetExecutorTest, SortsMultipleKeysAndKeepsEquivalentRowsStable) {
  auto group = Variable("group");
  auto rank = Variable("rank");
  ir::SortPlan plan(
      Arguments({"group", "rank", "id"}),
      {SortItem(group.get()),
       SortItem(rank.get(), ir::LogicalOrderDirection::kDescending)});
  const rg::QueryRows input = {{{"group", rg::Value("a")},
                                {"rank", rg::Value(1)},
                                {"id", rg::Value("first")}},
                               {{"group", rg::Value("b")},
                                {"rank", rg::Value(1)},
                                {"id", rg::Value("third")}},
                               {{"group", rg::Value("a")},
                                {"rank", rg::Value(2)},
                                {"id", rg::Value("highest")}},
                               {{"group", rg::Value("a")},
                                {"rank", rg::Value(1.0)},
                                {"id", rg::Value("second")}}};

  const rg::QueryRows rows = rg::ResultSetExecutor().Execute(plan, input);

  EXPECT_EQ(ColumnStrings(rows, "id"),
            (std::vector<std::string>{"highest", "first", "second", "third"}));
}

TEST(ResultSetExecutorTest, OrdersMixedValuesAndNullInBothDirections) {
  auto key = Variable("key");
  const rg::QueryRows input = {
      {{"key", rg::Value::Null()}, {"id", rg::Value("null")}},
      {{"key", rg::Value("x")}, {"id", rg::Value("string")}},
      {{"key", rg::Value(2)}, {"id", rg::Value("two")}},
      {{"key", rg::Value(1.5)}, {"id", rg::Value("fraction")}}};
  rg::ResultSetExecutor executor;

  ir::SortPlan ascending(Arguments({"key", "id"}), {SortItem(key.get())});
  EXPECT_EQ(ColumnStrings(executor.Execute(ascending, input), "id"),
            (std::vector<std::string>{"fraction", "two", "string", "null"}));

  ir::SortPlan descending(
      Arguments({"key", "id"}),
      {SortItem(key.get(), ir::LogicalOrderDirection::kDescending)});
  EXPECT_EQ(ColumnStrings(executor.Execute(descending, input), "id"),
            (std::vector<std::string>{"null", "string", "two", "fraction"}));
}

TEST(ResultSetExecutorTest, GivesNaNDeterministicStableOrdering) {
  auto key = Variable("key");
  ir::SortPlan plan(Arguments({"key", "id"}), {SortItem(key.get())});
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const rg::QueryRows input = {
      {{"key", rg::Value(nan)}, {"id", rg::Value("first nan")}},
      {{"key", rg::Value(1)}, {"id", rg::Value("finite")}},
      {{"key", rg::Value(nan)}, {"id", rg::Value("second nan")}}};

  const rg::QueryRows rows = rg::ResultSetExecutor().Execute(plan, input);

  EXPECT_EQ(ColumnStrings(rows, "id"),
            (std::vector<std::string>{"finite", "first nan", "second nan"}));
}

TEST(ResultSetExecutorTest, SkipsAndLimitsWithExactIntegerCounts) {
  const rg::QueryRows input = {{{"id", rg::Value(1)}},
                               {{"id", rg::Value(2)}},
                               {{"id", rg::Value(3)}},
                               {{"id", rg::Value(4)}}};
  auto two = Integer(2);
  auto three = Integer(3);
  ir::SkipPlan skip(Arguments({"id"}), two.get());
  ir::LimitPlan limit(Arguments({"id"}), three.get());
  rg::ResultSetExecutor executor;

  const rg::QueryRows skipped = executor.Execute(skip, input);
  const rg::QueryRows limited = executor.Execute(limit, input);

  ASSERT_EQ(skipped.size(), 2U);
  EXPECT_EQ(skipped[0].at("id"), rg::Value(3));
  ASSERT_EQ(limited.size(), 3U);
  EXPECT_EQ(limited.back().at("id"), rg::Value(3));
}

TEST(ResultSetExecutorTest, RejectsInvalidPaginationCountsOnEmptyInput) {
  auto negative = Integer(-1);
  auto fractional = std::make_unique<ast::DoubleLiteral>();
  fractional->value = 1.5;
  auto null = std::make_unique<ast::NullLiteral>();
  ir::SkipPlan negative_skip(Arguments({}), negative.get());
  ir::LimitPlan fractional_limit(Arguments({}), fractional.get());
  ir::LimitPlan null_limit(Arguments({}), null.get());
  rg::ResultSetExecutor executor;

  EXPECT_THROW((void)executor.Execute(negative_skip, {}),
               common::InvalidArgumentError);
  EXPECT_THROW((void)executor.Execute(fractional_limit, {}),
               common::InvalidArgumentError);
  EXPECT_THROW((void)executor.Execute(null_limit, {}),
               common::InvalidArgumentError);
}

TEST(ResultSetExecutorTest, OnlySkipsEmptyInputValidationWhenRowIsRequired) {
  auto exists = ExistsPattern();
  auto null = std::make_unique<ast::NullLiteral>();
  const std::vector<ir::LogicalPrecomputedExpression> precomputed = {
      {.expression = exists.get(), .variable = "computed"}};
  ir::LimitPlan dependent_limit(Arguments({}), exists.get(), precomputed);
  ir::LimitPlan constant_limit(Arguments({}), null.get(), precomputed);
  rg::ResultSetExecutor executor;

  EXPECT_TRUE(executor.Execute(dependent_limit, {}).empty());
  EXPECT_THROW((void)executor.Execute(constant_limit, {}),
               common::InvalidArgumentError);
}
