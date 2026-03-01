#include "ir/logical_planner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "common/exception.h"

namespace {

std::unique_ptr<ast::Statement> ParseOrFail(const std::string &query) {
  try {
    return ast::ParseCypherAndRewrite(query);
  } catch (const ast::ParseError &e) {
    ADD_FAILURE() << "parse errors for query: " << query
                  << " message: " << e.what();
  } catch (const ast::SemanticError &e) {
    ADD_FAILURE() << "semantic errors for query: " << query
                  << " message: " << e.what();
  }
  return {};
}

std::size_t CountNodeType(const std::vector<const ir::LogicalPlan *> &plans,
                          ir::LogicalPlanNodeType type) {
  return static_cast<std::size_t>(std::count_if(
      plans.begin(), plans.end(),
      [type](const ir::LogicalPlan *plan) { return plan->node_type == type; }));
}

TEST(LogicalPlannerTest, BuildsSinglePartPlanFromStatement) {
  auto statement = ParseOrFail("MATCH (a)-[r]->(b) RETURN a, b");
  ASSERT_TRUE(statement);

  auto plan = ir::BuildLogicalPlan(*statement);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  ASSERT_GE(flattened.size(), 4U);
  EXPECT_EQ(flattened[0]->node_type, ir::LogicalPlanNodeType::kProduceResult);
  EXPECT_EQ(flattened[1]->node_type, ir::LogicalPlanNodeType::kProject);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kExpand), 1U);
}

TEST(LogicalPlannerTest, PreservesSortSkipLimitPipeline) {
  auto statement = ParseOrFail(
      "MATCH (n)-[r]->(m) RETURN DISTINCT n AS x ORDER BY x SKIP 1 LIMIT 2");
  ASSERT_TRUE(statement);

  auto plan = ir::BuildLogicalPlan(*statement);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  ASSERT_GE(flattened.size(), 7U);
  EXPECT_EQ(flattened[0]->node_type, ir::LogicalPlanNodeType::kProduceResult);
  EXPECT_EQ(flattened[1]->node_type, ir::LogicalPlanNodeType::kLimit);
  EXPECT_EQ(flattened[2]->node_type, ir::LogicalPlanNodeType::kSkip);
  EXPECT_EQ(flattened[3]->node_type, ir::LogicalPlanNodeType::kSort);
  EXPECT_EQ(flattened[4]->node_type, ir::LogicalPlanNodeType::kProject);
}

TEST(LogicalPlannerTest, UsesCartesianProductForDisconnectedPattern) {
  auto statement =
      ParseOrFail("MATCH (a)-[r1:K1]->(b), (c)-[r2:K2]->(d) RETURN a, d");
  ASSERT_TRUE(statement);

  auto plan = ir::BuildLogicalPlan(*statement);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kExpand), 2U);
  EXPECT_EQ(
      CountNodeType(flattened, ir::LogicalPlanNodeType::kCartesianProduct), 1U);
}

TEST(LogicalPlannerTest, SupportsMultiPartQueryWithSymbolPropagation) {
  auto statement = ParseOrFail(
      "MATCH (n)-[r1]->(m) WHERE true AND true "
      "WITH n MATCH (n)-[r2]->(k) WHERE true AND true "
      "RETURN n, k");
  ASSERT_TRUE(statement);

  auto plan = ir::BuildLogicalPlan(*statement);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kExpand), 2U);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kAllNodesScan),
            1U);
}

TEST(LogicalPlannerTest, SupportsUnionQuery) {
  auto statement =
      ParseOrFail("MATCH (n) RETURN n AS x UNION MATCH (m) RETURN m AS x");
  ASSERT_TRUE(statement);

  auto plan = ir::BuildLogicalPlan(*statement);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(flattened[0]->node_type, ir::LogicalPlanNodeType::kProduceResult);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kUnion), 1U);

  const ir::Union *union_node = nullptr;
  for (const ir::LogicalPlan *node : flattened) {
    if (node->node_type == ir::LogicalPlanNodeType::kUnion) {
      union_node = static_cast<const ir::Union *>(node);
      break;
    }
  }
  ASSERT_NE(union_node, nullptr);
  EXPECT_FALSE(union_node->all);
}

TEST(LogicalPlannerTest, SupportsUnionAllQuery) {
  auto statement = ParseOrFail("RETURN 1 AS a UNION ALL RETURN 2 AS a");
  ASSERT_TRUE(statement);

  auto plan = ir::BuildLogicalPlan(*statement);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kUnion), 1U);

  const ir::Union *union_node = nullptr;
  for (const ir::LogicalPlan *node : flattened) {
    if (node->node_type == ir::LogicalPlanNodeType::kUnion) {
      union_node = static_cast<const ir::Union *>(node);
      break;
    }
  }
  ASSERT_NE(union_node, nullptr);
  EXPECT_TRUE(union_node->all);
}

TEST(LogicalPlannerTest, RejectsUnionWithDifferentProjectionColumns) {
  ast::IntegerLiteral one;
  one.value = 1;
  ast::IntegerLiteral two;
  two.value = 2;

  ir::QueryIR query_ir;
  query_ir.regular.main.projection.items.push_back({&one, "a"});

  ir::UnionBranch branch;
  branch.query.projection.items.push_back({&two, "b"});
  query_ir.regular.unions.push_back(std::move(branch));

  EXPECT_THROW((void)ir::BuildLogicalPlan(query_ir),
               common::InvalidArgumentError);
}

TEST(LogicalPlannerTest, RejectsProjectionWhereDependencyOutOfScope) {
  ast::IntegerLiteral one;
  one.value = 1;
  ast::Variable missing;
  missing.name = "missing";

  ir::QueryIR query_ir;
  query_ir.regular.main.projection.items.push_back({&one, "x"});
  query_ir.regular.main.projection.where = &missing;

  EXPECT_THROW((void)ir::BuildLogicalPlan(query_ir),
               common::InvalidArgumentError);
}

}  // namespace
