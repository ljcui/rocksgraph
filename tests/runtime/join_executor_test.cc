#include "runtime/join_executor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "value/value.h"

namespace {

std::unique_ptr<ast::Variable> Variable(std::string name) {
  auto variable = std::make_unique<ast::Variable>();
  variable->name = std::move(name);
  return variable;
}

std::unique_ptr<ast::ComparisonExpression> Comparison(std::string left,
                                                      std::string op,
                                                      std::string right) {
  auto comparison = std::make_unique<ast::ComparisonExpression>();
  comparison->left = Variable(std::move(left));
  comparison->op = std::move(op);
  comparison->right = Variable(std::move(right));
  return comparison;
}

std::unique_ptr<ir::ArgumentPlan> Arguments(std::vector<std::string> columns) {
  return std::make_unique<ir::ArgumentPlan>(std::move(columns));
}

const rg::Value &Column(const rg::QueryRow &row, const std::string &name) {
  return row.at(name);
}

}  // namespace

TEST(JoinExecutorTest, CartesianProductPreservesDuplicatesAndRejectsConflicts) {
  rg::JoinExecutor executor;
  ir::CartesianProductPlan plan(Arguments({"left"}), Arguments({"right"}));
  const rg::QueryRows rows = executor.Execute(
      plan, {{{"left", rg::Value(1)}}, {{"left", rg::Value(1)}}},
      {{{"right", rg::Value("a")}}, {{"right", rg::Value("b")}}});

  ASSERT_EQ(rows.size(), 4U);
  EXPECT_EQ(Column(rows[0], "right"), rg::Value("a"));
  EXPECT_EQ(Column(rows[1], "right"), rg::Value("b"));
  EXPECT_EQ(Column(rows[2], "right"), rg::Value("a"));
  EXPECT_EQ(Column(rows[3], "right"), rg::Value("b"));

  ir::CartesianProductPlan conflict_plan(Arguments({"value"}),
                                         Arguments({"value"}));
  EXPECT_TRUE(executor
                  .Execute(conflict_plan, {{{"value", rg::Value(1)}}},
                           {{{"value", rg::Value(2)}}})
                  .empty());
}

TEST(JoinExecutorTest, NodeHashJoinNormalizesNumbersAndSkipsNullKeys) {
  rg::JoinExecutor executor;
  ir::NodeHashJoinPlan plan(Arguments({"key", "left"}),
                            Arguments({"key", "right"}), {"key"});
  const rg::QueryRows rows = executor.Execute(
      plan,
      {{{"key", rg::Value(1)}, {"left", rg::Value("one")}},
       {{"key", rg::Value::Null()}, {"left", rg::Value("null")}}},
      {{{"key", rg::Value(1.0)}, {"right", rg::Value("first")}},
       {{"key", rg::Value(1)}, {"right", rg::Value("second")}},
       {{"key", rg::Value::Null()}, {"right", rg::Value("null")}}});

  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(Column(rows[0], "left"), rg::Value("one"));
  EXPECT_EQ(Column(rows[0], "right"), rg::Value("first"));
  EXPECT_EQ(Column(rows[1], "right"), rg::Value("second"));
}

TEST(JoinExecutorTest, NodeHashJoinUsesAllKeysAndChecksSharedColumns) {
  rg::JoinExecutor executor;
  ir::NodeHashJoinPlan plan(Arguments({"first", "second", "shared", "left"}),
                            Arguments({"first", "second", "shared", "right"}),
                            {"first", "second"});
  const rg::QueryRows rows =
      executor.Execute(plan,
                       {{{"first", rg::Value(1)},
                         {"second", rg::Value("x")},
                         {"shared", rg::Value(true)},
                         {"left", rg::Value("match")}}},
                       {{{"first", rg::Value(1.0)},
                         {"second", rg::Value("x")},
                         {"shared", rg::Value(true)},
                         {"right", rg::Value("kept")}},
                        {{"first", rg::Value(1)},
                         {"second", rg::Value("y")},
                         {"shared", rg::Value(true)},
                         {"right", rg::Value("wrong key")}},
                        {{"first", rg::Value(1)},
                         {"second", rg::Value("x")},
                         {"shared", rg::Value(false)},
                         {"right", rg::Value("conflict")}}});

  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(Column(rows[0], "right"), rg::Value("kept"));
}

TEST(JoinExecutorTest, NodeHashJoinUsesGraphEntityIdentity) {
  auto left_node = std::make_shared<rg::Node>();
  left_node->id = 7;
  left_node->labels = {"Before"};
  auto right_node = std::make_shared<rg::Node>();
  right_node->id = 7;
  right_node->labels = {"After"};

  rg::JoinExecutor executor;
  ir::NodeHashJoinPlan plan(Arguments({"node", "left"}),
                            Arguments({"node", "right"}), {"node"});
  const rg::QueryRows rows = executor.Execute(
      plan, {{{"node", rg::Value(left_node)}, {"left", rg::Value(1)}}},
      {{{"node", rg::Value(right_node)}, {"right", rg::Value(2)}}});

  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(Column(rows[0], "left"), rg::Value(1));
  EXPECT_EQ(Column(rows[0], "right"), rg::Value(2));
}

TEST(JoinExecutorTest, ValueHashJoinRejectsFalseAndNullPredicates) {
  auto predicate = Comparison("left", "=", "right");
  rg::JoinExecutor executor;
  ir::ValueHashJoinPlan plan(Arguments({"left"}), Arguments({"right"}),
                             {predicate.get()});
  const rg::QueryRows rows = executor.Execute(
      plan, {{{"left", rg::Value(1)}}, {{"left", rg::Value::Null()}}},
      {{{"right", rg::Value(1.0)}},
       {{"right", rg::Value(2)}},
       {{"right", rg::Value::Null()}}});

  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(Column(rows[0], "left"), rg::Value(1));
  EXPECT_EQ(Column(rows[0], "right"), rg::Value(1.0));
}

TEST(JoinExecutorTest, PredicateJoinEvaluatesAllPredicates) {
  auto greater = Comparison("left", ">", "right");
  auto unequal = Comparison("left", "<>", "right");
  rg::JoinExecutor executor;
  ir::PredicateJoinPlan plan(Arguments({"left"}), Arguments({"right"}),
                             {greater.get(), unequal.get()});
  const rg::QueryRows rows = executor.Execute(
      plan, {{{"left", rg::Value(3)}}, {{"left", rg::Value::Null()}}},
      {{{"right", rg::Value(2)}}, {{"right", rg::Value(4)}}});

  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(Column(rows[0], "left"), rg::Value(3));
  EXPECT_EQ(Column(rows[0], "right"), rg::Value(2));
}
