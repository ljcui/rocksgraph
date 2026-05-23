#include "ir/logical_plan_builder.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "common/exception.h"
#include "ir/logical_plan_printer.h"
#include "ir/planner_query.h"

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

std::string LogicalPlanText(const std::string &query) {
  auto statement = ParseOrFail(query);
  if (!statement) {
    return {};
  }
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan =
      ir::CreateLogicalPlan(*planner_query);
  return ir::LogicalPlanToString(*logical_plan);
}

void ExpectLogicalPlanText(const std::string &query,
                           const std::string &expected) {
  EXPECT_EQ(LogicalPlanText(query), expected);
}

}  // namespace

TEST(LogicalPlanBuilderTest, BuildsAllNodeScanProjectionAndResults) {
  ExpectLogicalPlanText("MATCH (n) RETURN n", R"(ProduceResults [n]
  Projection [n]
    AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, UsesNodeLabelPredicateAsLeafScan) {
  ExpectLogicalPlanText("MATCH (n:Person) RETURN n", R"(ProduceResults [n]
  Projection [n]
    NodeByLabelScan [n:Person]
)");
}

TEST(LogicalPlanBuilderTest, BuildsExpandFromSingleRelationshipPattern) {
  ExpectLogicalPlanText("MATCH (a)-[r:KNOWS]->(b) RETURN a, r, b",
                        R"(ProduceResults [a, r, b]
  Projection [a, r, b]
    Expand [(a)-[r:KNOWS]->(b)]
      AllNodeScan [a]
)");
}

TEST(LogicalPlanBuilderTest, PushesAvailableWherePredicatesIntoFilters) {
  ExpectLogicalPlanText(
      "MATCH (a:Person)-[r:KNOWS]->(b) WHERE b.name = 'Ada' RETURN a, b",
      R"(ProduceResults [a, b]
  Projection [a, b]
    Filter [b.name = 'Ada']
      Expand [(a)-[r:KNOWS]->(b)]
        NodeByLabelScan [a:Person]
)");
}

TEST(LogicalPlanBuilderTest, BuildsCartesianProductForDisconnectedComponents) {
  ExpectLogicalPlanText("MATCH (a), (b) RETURN a, b", R"(ProduceResults [a, b]
  Projection [a, b]
    CartesianProduct
      AllNodeScan [a]
      AllNodeScan [b]
)");
}

TEST(LogicalPlanBuilderTest, RejectsUnsupportedHorizon) {
  auto statement = ParseOrFail("MATCH (n) RETURN DISTINCT n");
  ASSERT_TRUE(statement);
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  EXPECT_THROW((void)ir::CreateLogicalPlan(*planner_query),
               common::InvalidArgumentError);
}
