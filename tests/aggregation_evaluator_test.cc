#include "runtime/aggregation_evaluator.h"

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

std::unique_ptr<ast::Variable> Variable(std::string name) {
  auto variable = std::make_unique<ast::Variable>();
  variable->name = std::move(name);
  return variable;
}

ast::FunctionInvocation Function(std::string name, std::string argument,
                                 bool distinct = false) {
  ast::FunctionInvocation function;
  function.function_name = std::move(name);
  function.distinct = distinct;
  function.arguments.push_back(Variable(std::move(argument)));
  return function;
}

ast::FunctionInvocation PercentileFunction(std::string name, double percentile,
                                           bool distinct = false) {
  ast::FunctionInvocation function =
      Function(std::move(name), "value", distinct);
  auto argument = std::make_unique<ast::DoubleLiteral>();
  argument->value = percentile;
  function.arguments.push_back(std::move(argument));
  return function;
}

const rg::Value &Column(const rg::QueryRow &row, const std::string &name) {
  return row.at(name);
}

}  // namespace

TEST(AggregationEvaluatorTest, GroupsRowsAndEvaluatesAggregateFunctions) {
  ast::Variable group;
  group.name = "group";
  ast::CountStarExpression count_star;
  ast::FunctionInvocation count = Function("count", "value");
  ast::FunctionInvocation collect = Function("collect", "value");
  ast::FunctionInvocation sum = Function("sum", "value");
  ast::FunctionInvocation average = Function("avg", "value");
  ast::FunctionInvocation minimum = Function("min", "value");
  ast::FunctionInvocation maximum = Function("max", "value");

  const std::vector<rg::QueryRow> rows = {
      {{"group", rg::Value("a")}, {"value", rg::Value(1)}},
      {{"group", rg::Value("a")}, {"value", rg::Value(2)}},
      {{"group", rg::Value("a")}, {"value", rg::Value::Null()}},
      {{"group", rg::Value("b")}, {"value", rg::Value(4)}}};
  const std::vector<ir::LogicalProjectionItem> grouping_items = {
      {.expression = &group, .alias = "group"}};
  const std::vector<ir::LogicalProjectionItem> aggregation_items = {
      {.expression = &count_star, .alias = "rows"},
      {.expression = &count, .alias = "count"},
      {.expression = &collect, .alias = "values"},
      {.expression = &sum, .alias = "sum"},
      {.expression = &average, .alias = "average"},
      {.expression = &minimum, .alias = "minimum"},
      {.expression = &maximum, .alias = "maximum"}};

  const std::vector<rg::QueryRow> result =
      rg::AggregateRows(grouping_items, aggregation_items, rows);

  ASSERT_EQ(result.size(), 2U);
  EXPECT_EQ(Column(result[0], "group"), rg::Value("a"));
  EXPECT_EQ(Column(result[0], "rows"), rg::Value(3));
  EXPECT_EQ(Column(result[0], "count"), rg::Value(2));
  EXPECT_EQ(Column(result[0], "values"),
            rg::Value(rg::Value::List{rg::Value(1), rg::Value(2)}));
  EXPECT_EQ(Column(result[0], "sum"), rg::Value(3));
  EXPECT_EQ(Column(result[0], "average"), rg::Value(1.5));
  EXPECT_EQ(Column(result[0], "minimum"), rg::Value(1));
  EXPECT_EQ(Column(result[0], "maximum"), rg::Value(2));
  EXPECT_EQ(Column(result[1], "group"), rg::Value("b"));
  EXPECT_EQ(Column(result[1], "rows"), rg::Value(1));
}

TEST(AggregationEvaluatorTest, UsesValueSemanticsForGroupingAndDistinct) {
  ast::Variable value;
  value.name = "value";
  ast::CountStarExpression count_star;

  const std::vector<rg::QueryRow> rows = {
      {{"value", rg::Value(rg::Value::List{rg::Value(1)})}},
      {{"value", rg::Value(rg::Value::List{rg::Value(1.0)})}},
      {{"value", rg::Value(rg::Value::List{rg::Value(2)})}}};
  const std::vector<ir::LogicalProjectionItem> grouping_items = {
      {.expression = &value, .alias = "value"}};

  const std::vector<rg::QueryRow> distinct =
      rg::ProjectDistinctRows(grouping_items, rows);
  const std::vector<rg::QueryRow> grouped = rg::AggregateRows(
      grouping_items, {{.expression = &count_star, .alias = "count"}}, rows);

  ASSERT_EQ(distinct.size(), 2U);
  EXPECT_EQ(Column(distinct[0], "value"),
            rg::Value(rg::Value::List{rg::Value(1)}));
  EXPECT_EQ(Column(distinct[1], "value"),
            rg::Value(rg::Value::List{rg::Value(2)}));
  ASSERT_EQ(grouped.size(), 2U);
  EXPECT_EQ(Column(grouped[0], "count"), rg::Value(2));
  EXPECT_EQ(Column(grouped[1], "count"), rg::Value(1));
}

TEST(AggregationEvaluatorTest, AppliesDistinctAfterIgnoringNullValues) {
  ast::FunctionInvocation count = Function("count", "value", true);
  ast::FunctionInvocation collect = Function("collect", "value", true);
  const std::vector<rg::QueryRow> rows = {
      {{"value", rg::Value(rg::Value::List{rg::Value(1)})}},
      {{"value", rg::Value(rg::Value::List{rg::Value(1.0)})}},
      {{"value", rg::Value(rg::Value::List{rg::Value(2)})}},
      {{"value", rg::Value::Null()}}};
  const std::vector<ir::LogicalProjectionItem> aggregation_items = {
      {.expression = &count, .alias = "count"},
      {.expression = &collect, .alias = "values"}};

  const std::vector<rg::QueryRow> result =
      rg::AggregateRows({}, aggregation_items, rows);

  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(Column(result[0], "count"), rg::Value(2));
  EXPECT_EQ(
      Column(result[0], "values"),
      rg::Value(rg::Value::List{rg::Value(rg::Value::List{rg::Value(1)}),
                                rg::Value(rg::Value::List{rg::Value(2)})}));
}

TEST(AggregationEvaluatorTest, EvaluatesDiscreteAndContinuousPercentiles) {
  ast::FunctionInvocation discrete = PercentileFunction("percentileDisc", 0.5);
  ast::FunctionInvocation continuous =
      PercentileFunction("percentileCont", 0.25);
  const std::vector<rg::QueryRow> rows = {{{"value", rg::Value(40)}},
                                          {{"value", rg::Value(10)}},
                                          {{"value", rg::Value::Null()}},
                                          {{"value", rg::Value(30)}},
                                          {{"value", rg::Value(20)}}};

  const std::vector<rg::QueryRow> result =
      rg::AggregateRows({},
                        {{.expression = &discrete, .alias = "discrete"},
                         {.expression = &continuous, .alias = "continuous"}},
                        rows);

  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(Column(result[0], "discrete"), rg::Value(20));
  EXPECT_EQ(Column(result[0], "continuous"), rg::Value(17.5));
}

TEST(AggregationEvaluatorTest, ProducesGlobalAggregateRowForEmptyInput) {
  ast::CountStarExpression count_star;
  ast::FunctionInvocation count = Function("count", "value");
  ast::FunctionInvocation collect = Function("collect", "value");
  ast::FunctionInvocation sum = Function("sum", "value");
  ast::FunctionInvocation average = Function("avg", "value");
  ast::FunctionInvocation minimum = Function("min", "value");
  ast::FunctionInvocation maximum = Function("max", "value");
  ast::FunctionInvocation percentile_discrete =
      PercentileFunction("percentileDisc", 0.5);
  ast::FunctionInvocation percentile_continuous =
      PercentileFunction("percentileCont", 0.5);
  const std::vector<ir::LogicalProjectionItem> aggregation_items = {
      {.expression = &count_star, .alias = "rows"},
      {.expression = &count, .alias = "count"},
      {.expression = &collect, .alias = "values"},
      {.expression = &sum, .alias = "sum"},
      {.expression = &average, .alias = "average"},
      {.expression = &minimum, .alias = "minimum"},
      {.expression = &maximum, .alias = "maximum"},
      {.expression = &percentile_discrete, .alias = "percentile_discrete"},
      {.expression = &percentile_continuous, .alias = "percentile_continuous"}};

  const std::vector<rg::QueryRow> result =
      rg::AggregateRows({}, aggregation_items, {});

  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(Column(result[0], "rows"), rg::Value(0));
  EXPECT_EQ(Column(result[0], "count"), rg::Value(0));
  EXPECT_EQ(Column(result[0], "values"), rg::Value(rg::Value::List{}));
  EXPECT_EQ(Column(result[0], "sum"), rg::Value(0));
  EXPECT_TRUE(Column(result[0], "average").IsNull());
  EXPECT_TRUE(Column(result[0], "minimum").IsNull());
  EXPECT_TRUE(Column(result[0], "maximum").IsNull());
  EXPECT_TRUE(Column(result[0], "percentile_discrete").IsNull());
  EXPECT_TRUE(Column(result[0], "percentile_continuous").IsNull());
}

TEST(AggregationEvaluatorTest, RejectsInvalidValuesAndIntegerOverflow) {
  ast::FunctionInvocation sum = Function("sum", "value");
  const std::vector<ir::LogicalProjectionItem> aggregation_items = {
      {.expression = &sum, .alias = "sum"}};

  EXPECT_THROW(
      (void)rg::AggregateRows(
          {}, aggregation_items,
          {{{"value", rg::Value(std::numeric_limits<std::int64_t>::max())}},
           {{"value", rg::Value(1)}}}),
      common::InvalidArgumentError);
  EXPECT_THROW((void)rg::AggregateRows({}, aggregation_items,
                                       {{{"value", rg::Value("not numeric")}}}),
               common::InvalidArgumentError);
}

TEST(AggregationEvaluatorTest, RejectsInvalidPercentileArguments) {
  ast::FunctionInvocation out_of_range =
      PercentileFunction("percentileCont", 1.1);
  ast::FunctionInvocation varying = Function("percentileDisc", "value");
  varying.arguments.push_back(Variable("percentile"));

  EXPECT_THROW((void)rg::AggregateRows(
                   {}, {{.expression = &out_of_range, .alias = "value"}},
                   {{{"value", rg::Value(1)}}}),
               common::InvalidArgumentError);
  EXPECT_THROW(
      (void)rg::AggregateRows(
          {}, {{.expression = &varying, .alias = "value"}},
          {{{"value", rg::Value(1)}, {"percentile", rg::Value(0.25)}},
           {{"value", rg::Value(2)}, {"percentile", rg::Value(0.75)}}}),
      common::InvalidArgumentError);
}

TEST(AggregationEvaluatorTest, ValidatesRegisteredFunctionContract) {
  ast::FunctionInvocation unknown = Function("unknown", "value");
  ast::FunctionInvocation scalar = Function("size", "value");
  ast::FunctionInvocation count_without_argument;
  count_without_argument.function_name = "count";
  const std::vector<rg::QueryRow> rows = {{{"value", rg::Value(1)}}};

  EXPECT_THROW((void)rg::AggregateRows(
                   {}, {{.expression = &unknown, .alias = "value"}}, rows),
               common::InvalidArgumentError);
  EXPECT_THROW((void)rg::AggregateRows(
                   {}, {{.expression = &scalar, .alias = "value"}}, rows),
               common::InvalidArgumentError);
  EXPECT_THROW(
      (void)rg::AggregateRows(
          {}, {{.expression = &count_without_argument, .alias = "value"}},
          rows),
      common::InvalidArgumentError);
}
