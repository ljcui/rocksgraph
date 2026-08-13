#include "runtime/row_operator_executor.h"

#include <gtest/gtest.h>

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

const rg::Value &Column(const rg::QueryRow &row, const std::string &name) {
  return row.at(name);
}

}  // namespace

TEST(RowOperatorExecutorTest, ArgumentProjectsBoundColumns) {
  rg::RowOperatorExecutor executor;
  ir::ArgumentPlan projected({"keep"});
  const rg::QueryRows input = {
      {{"keep", rg::Value(1)}, {"drop", rg::Value(2)}}};

  const rg::QueryRows rows = executor.Execute(projected, input);

  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0], rg::QueryRow({{"keep", rg::Value(1)}}));

  ir::ArgumentPlan preserve_all({});
  EXPECT_EQ(executor.Execute(preserve_all, input), input);
  EXPECT_THROW((void)executor.Execute(projected, {{{"drop", rg::Value(2)}}}),
               common::InvalidArgumentError);
}

TEST(RowOperatorExecutorTest, FiltersAndProjectsRows) {
  auto selected = Variable("selected");
  ir::FilterPlan filter(Arguments({"selected", "value"}), selected.get());
  const rg::QueryRows input = {
      {{"selected", rg::Value(true)}, {"value", rg::Value("first")}},
      {{"selected", rg::Value(false)}, {"value", rg::Value("second")}},
      {{"selected", rg::Value::Null()}, {"value", rg::Value("third")}}};
  rg::RowOperatorExecutor executor;

  const rg::QueryRows filtered = executor.Execute(filter, input);

  ASSERT_EQ(filtered.size(), 1U);
  EXPECT_EQ(Column(filtered[0], "value"), rg::Value("first"));

  auto value = Variable("value");
  ir::ProjectionPlan projection(Arguments({"selected", "value"}),
                                {{.alias = "selected", .passthrough = true},
                                 {.expression = value.get(), .alias = "copy"}});
  const rg::QueryRows projected = executor.Execute(projection, filtered);
  ASSERT_EQ(projected.size(), 1U);
  EXPECT_EQ(projected[0], rg::QueryRow({{"copy", rg::Value("first")},
                                        {"selected", rg::Value(true)}}));

  ir::ProjectionPlan missing_passthrough(
      Arguments({}), {{.alias = "missing", .passthrough = true}});
  EXPECT_THROW((void)executor.Execute(missing_passthrough, filtered),
               common::InvalidArgumentError);
}

TEST(RowOperatorExecutorTest, ExecutesDistinctAndAggregation) {
  auto group = Variable("group");
  const ir::LogicalProjectionItem grouping = {.expression = group.get(),
                                              .alias = "group"};
  const rg::QueryRows input = {{{"group", rg::Value("a")}},
                               {{"group", rg::Value("a")}},
                               {{"group", rg::Value("b")}}};
  rg::RowOperatorExecutor executor;

  ir::DistinctPlan distinct(Arguments({"group"}), {grouping});
  const rg::QueryRows distinct_rows = executor.Execute(distinct, input);
  ASSERT_EQ(distinct_rows.size(), 2U);
  EXPECT_EQ(Column(distinct_rows[0], "group"), rg::Value("a"));
  EXPECT_EQ(Column(distinct_rows[1], "group"), rg::Value("b"));

  ast::CountStarExpression count;
  ir::AggregationPlan aggregation(Arguments({"group"}), {grouping},
                                  {{.expression = &count, .alias = "count"}});
  const rg::QueryRows aggregated = executor.Execute(aggregation, input);
  ASSERT_EQ(aggregated.size(), 2U);
  EXPECT_EQ(Column(aggregated[0], "count"), rg::Value(2));
  EXPECT_EQ(Column(aggregated[1], "count"), rg::Value(1));
}

TEST(RowOperatorExecutorTest, UnwindsListsAndSkipsOtherValues) {
  auto items = Variable("items");
  ir::UnwindPlan unwind(Arguments({"id", "items"}), items.get(), "item");
  const rg::QueryRows input = {
      {{"id", rg::Value("list")},
       {"items", rg::Value(rg::Value::List{rg::Value(1), rg::Value(2)})}},
      {{"id", rg::Value("scalar")}, {"items", rg::Value(3)}}};

  const rg::QueryRows rows = rg::RowOperatorExecutor().Execute(unwind, input);

  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(Column(rows[0], "id"), rg::Value("list"));
  EXPECT_EQ(Column(rows[0], "item"), rg::Value(1));
  EXPECT_EQ(Column(rows[1], "item"), rg::Value(2));
}

TEST(RowOperatorExecutorTest, MapsAndDeduplicatesUnionRows) {
  const std::vector<ir::LogicalUnionMapping> mappings = {
      {.output_variable = "value",
       .lhs_variable = "left",
       .rhs_variable = "right"}};
  const rg::QueryRows left = {{{"left", rg::Value(1)}},
                              {{"left", rg::Value(2)}}};
  const rg::QueryRows right = {{{"right", rg::Value(1)}},
                               {{"right", rg::Value(3)}}};
  rg::RowOperatorExecutor executor;

  ir::UnionPlan distinct(Arguments({"left"}), Arguments({"right"}), mappings,
                         false);
  const rg::QueryRows distinct_rows = executor.Execute(distinct, left, right);
  ASSERT_EQ(distinct_rows.size(), 3U);
  EXPECT_EQ(Column(distinct_rows[0], "value"), rg::Value(1));
  EXPECT_EQ(Column(distinct_rows[1], "value"), rg::Value(2));
  EXPECT_EQ(Column(distinct_rows[2], "value"), rg::Value(3));

  ir::UnionPlan all(Arguments({"left"}), Arguments({"right"}), mappings, true);
  EXPECT_EQ(executor.Execute(all, left, right).size(), 4U);
  EXPECT_THROW(
      (void)executor.Execute(distinct, {{{"missing", rg::Value(1)}}}, right),
      common::InvalidArgumentError);
}
