#include "ir/logical_plan_builder.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "common/exception.h"
#include "ir/logical_plan_printer.h"
#include "ir/planner_query.h"
#include "tests/fake_planner_statistics.h"

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

std::string LogicalPlanText(const std::string &query,
                            const ir::LogicalPlanBuilderOptions &options = {}) {
  auto statement = ParseOrFail(query);
  if (!statement) {
    return {};
  }
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan =
      ir::CreateLogicalPlan(*planner_query, options);
  return ir::LogicalPlanToString(*logical_plan);
}

void ExpectLogicalPlanText(const std::string &query,
                           const std::string &expected,
                           const ir::LogicalPlanBuilderOptions &options = {}) {
  EXPECT_EQ(LogicalPlanText(query, options), expected);
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

TEST(LogicalPlanBuilderTest, IdpChoosesCheaperLabelLeaf) {
  ExpectLogicalPlanText(
      "MATCH (a)-[r]->(b:Person) RETURN a, b",
      R"(ProduceResults [a, b]
  Projection [a, b]
    Expand [(b)<-[r]-(a)]
      NodeByLabelScan [b:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, IdpPruningKeepsCheapestLeafCandidate) {
  ExpectLogicalPlanText(
      "MATCH (a)-[r]->(b:Person) RETURN a, b",
      R"(ProduceResults [a, b]
  Projection [a, b]
    Expand [(b)<-[r]-(a)]
      NodeByLabelScan [b:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp,
          .max_idp_candidates_per_relationship_count = 1});
}

TEST(LogicalPlanBuilderTest, IdpUsesInjectedStatisticsForLeafCost) {
  test_support::FakePlannerStatistics statistics;
  statistics.node_count_by_label = {{"Rare", 5.0}, {"Common", 500.0}};
  ExpectLogicalPlanText(
      "MATCH (a:Common)-[r]->(b:Rare) RETURN a, b",
      R"(ProduceResults [a, b]
  Projection [a, b]
    Filter [a:Common]
      Expand [(b)<-[r]-(a)]
        NodeByLabelScan [b:Rare]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp,
          .planner_statistics = &statistics});
}

TEST(LogicalPlanBuilderTest, IdpBuildsTwoHopJoinFromCheaperMiddleLeaf) {
  ExpectLogicalPlanText(
      "MATCH (a)-[r1]->(b:Person)-[r2]->(c) RETURN a, b, c",
      R"(ProduceResults [a, b, c]
  Projection [a, b, c]
    Filter [NOT (r1 = r2)]
      NodeHashJoin [b]
        Expand [(b)<-[r1]-(a)]
          NodeByLabelScan [b:Person]
        Expand [(b)-[r2]->(c)]
          NodeByLabelScan [b:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, IdpKeepsDistinctLeafCandidatesWithSameCost) {
  ExpectLogicalPlanText(
      "MATCH (a:Person)-[r1]->(b)-[r2:KNOWS]->(c:Person) RETURN a, b, c",
      R"(ProduceResults [a, b, c]
  Projection [a, b, c]
    Filter [NOT (r1 = r2)]
      NodeHashJoin [b]
        Expand [(a)-[r1]->(b)]
          NodeByLabelScan [a:Person]
        Expand [(c)<-[r2:KNOWS]-(b)]
          NodeByLabelScan [c:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, IdpBuildsNodeHashJoinForSharedNodeSubplans) {
  ExpectLogicalPlanText(
      "MATCH (a:Person)-[r1]->(b)<-[r2]-(c:Person) RETURN a, b, c",
      R"(ProduceResults [a, b, c]
  Projection [a, b, c]
    Filter [NOT (r1 = r2)]
      NodeHashJoin [b]
        Expand [(a)-[r1]->(b)]
          NodeByLabelScan [a:Person]
        Expand [(c)-[r2]->(b)]
          NodeByLabelScan [c:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, IdpExtendsThreeRelationshipJoinedSubplan) {
  ExpectLogicalPlanText(
      "MATCH (a:Person)-[r1]->(b)<-[r2]-(c:Person), "
      "(b)-[r3]->(d:Person) RETURN a, b, c, d",
      R"(ProduceResults [a, b, c, d]
  Projection [a, b, c, d]
    Filter [d:Person]
      Filter [NOT (r2 = r3)]
        Filter [NOT (r1 = r3)]
          Expand [(b)-[r3]->(d)]
            Filter [NOT (r1 = r2)]
              NodeHashJoin [b]
                Expand [(a)-[r1]->(b)]
                  NodeByLabelScan [a:Person]
                Expand [(c)-[r2]->(b)]
                  NodeByLabelScan [c:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, IdpBuildsNestedThreeRelationshipBushyJoinPlan) {
  ExpectLogicalPlanText(
      "MATCH (a:Person)-[r1]->(b) "
      "MATCH (b)<-[r2]-(c:Person) "
      "MATCH (b)-[r3]->(d:Person) RETURN a, b, c, d",
      R"(ProduceResults [a, b, c, d]
  Projection [a, b, c, d]
    NodeHashJoin [b]
      Expand [(a)-[r1]->(b)]
        NodeByLabelScan [a:Person]
      NodeHashJoin [b]
        Expand [(c)-[r2]->(b)]
          NodeByLabelScan [c:Person]
        Expand [(d)<-[r3]-(b)]
          NodeByLabelScan [d:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, IdpPlansTriangleWithExpandIntoCompetition) {
  ExpectLogicalPlanText(
      "MATCH (a:Person)-[r1]->(b), (b)-[r2]->(c), (a)-[r3]->(c) "
      "RETURN a, b, c",
      R"(ProduceResults [a, b, c]
  Projection [a, b, c]
    Filter [NOT (r2 = r3)]
      Filter [NOT (r1 = r2)]
        ExpandInto [(b)-[r2]->(c)]
          Filter [NOT (r1 = r3)]
            NodeHashJoin [a]
              Expand [(a)-[r1]->(b)]
                NodeByLabelScan [a:Person]
              Expand [(a)-[r3]->(c)]
                NodeByLabelScan [a:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, IdpBuildsExpandIntoForBoundEndpoints) {
  ExpectLogicalPlanText(
      "MATCH (a)-[r1]->(b:Person), (a)-[r2]->(b) RETURN a, b",
      R"(ProduceResults [a, b]
  Projection [a, b]
    Filter [NOT (r1 = r2)]
      ExpandInto [(a)-[r2]->(b)]
        Expand [(b)<-[r1]-(a)]
          NodeByLabelScan [b:Person]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, BuildsExpandIntoForAlreadyBoundEndpoints) {
  ExpectLogicalPlanText("MATCH (a)-[r1]->(b), (a)-[r2]->(b) RETURN a, b",
                        R"(ProduceResults [a, b]
  Projection [a, b]
    Filter [NOT (r1 = r2)]
      ExpandInto [(a)-[r2]->(b)]
        Expand [(a)-[r1]->(b)]
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

TEST(LogicalPlanBuilderTest, BuildsOrderByPlan) {
  ExpectLogicalPlanText("MATCH (n) RETURN n ORDER BY n.name",
                        R"(ProduceResults [n]
  Sort [n.name ASC]
    Projection [n]
      AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsSkipAndLimitPlans) {
  ExpectLogicalPlanText("MATCH (n) RETURN n SKIP 10 LIMIT 20",
                        R"(ProduceResults [n]
  Limit [20]
    Skip [10]
      Projection [n]
        AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsOrderBySkipAndLimitPlans) {
  ExpectLogicalPlanText(
      "MATCH (n) RETURN n ORDER BY n.name DESC SKIP 5 LIMIT 10",
      R"(ProduceResults [n]
  Limit [10]
    Skip [5]
      Sort [n.name DESC]
        Projection [n]
          AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, RewritesOrderByProjectionExpressionToAlias) {
  ExpectLogicalPlanText("MATCH (n) RETURN n.age AS age ORDER BY n.age",
                        R"(ProduceResults [age]
  Sort [age ASC]
    Projection [age]
      AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsDistinctPlan) {
  ExpectLogicalPlanText("MATCH (n) RETURN DISTINCT n.name AS name",
                        R"(ProduceResults [name]
  Distinct [name]
    AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsDistinctOrderByPlan) {
  ExpectLogicalPlanText("MATCH (n) RETURN DISTINCT n ORDER BY n.name",
                        R"(ProduceResults [n]
  Sort [n.name ASC]
    Distinct [n]
      AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsDistinctOrderBySkipAndLimitPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) RETURN DISTINCT n.name AS name ORDER BY n.name DESC SKIP 1 "
      "LIMIT 2",
      R"(ProduceResults [name]
  Limit [2]
    Skip [1]
      Sort [name DESC]
        Distinct [name]
          AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsAggregationPlan) {
  ExpectLogicalPlanText("MATCH (n) RETURN count(n) AS c",
                        R"(ProduceResults [c]
  Aggregation [c]
    AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsGroupingAggregationPlan) {
  ExpectLogicalPlanText("MATCH (n) RETURN n.name AS name, count(*) AS c",
                        R"(ProduceResults [name, c]
  Aggregation [name, c]
    AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsAggregationOrderBySkipAndLimitPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) RETURN n.name AS name, count(*) AS c ORDER BY n.name DESC "
      "SKIP 1 LIMIT 2",
      R"(ProduceResults [name, c]
  Limit [2]
    Skip [1]
      Sort [name DESC]
        Aggregation [name, c]
          AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsProjectionOnlyTailPlan) {
  ExpectLogicalPlanText("MATCH (n) WITH n.name AS name RETURN name",
                        R"(ProduceResults [name]
  Projection [name]
    Projection [name]
      AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsDistinctProjectionOnlyTailPlan) {
  ExpectLogicalPlanText("MATCH (n) WITH DISTINCT n RETURN n",
                        R"(ProduceResults [n]
  Projection [n]
    Distinct [n]
      AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsTailMatchWithArgumentExpandPlan) {
  ExpectLogicalPlanText("MATCH (n) WITH DISTINCT n MATCH (n)-->(m) RETURN m",
                        R"(ProduceResults [m]
  Projection [m]
    Apply
      Distinct [n]
        AllNodeScan [n]
      Expand [(n)-[anon_0]->(m)]
        Argument [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsTailMatchWithBoundEndpointsExpandIntoPlan) {
  ExpectLogicalPlanText(
      "MATCH (a), (b) WITH DISTINCT a, b MATCH (a)-[r]->(b) RETURN a, b",
      R"(ProduceResults [a, b]
  Projection [a, b]
    Apply
      Distinct [a, b]
        CartesianProduct
          AllNodeScan [a]
          AllNodeScan [b]
      ExpandInto [(a)-[r]->(b)]
        Argument [a, b]
)");
}

TEST(LogicalPlanBuilderTest, IdpKeepsTailMatchArgumentDependency) {
  ExpectLogicalPlanText(
      "MATCH (n) WITH DISTINCT n MATCH (n)-[r]->(m:Person) RETURN m",
      R"(ProduceResults [m]
  Projection [m]
    Apply
      Distinct [n]
        AllNodeScan [n]
      Filter [m:Person]
        Expand [(n)-[r]->(m)]
          Argument [n]
)",
      ir::LogicalPlanBuilderOptions{
          .component_planner = ir::LogicalPlanComponentPlannerKind::kIdp});
}

TEST(LogicalPlanBuilderTest, BuildsTailMatchWithoutArgumentDependencyPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) WITH n.name AS name MATCH (m) RETURN name, m",
      R"(ProduceResults [name, m]
  Projection [name, m]
    Apply
      Projection [name]
        AllNodeScan [n]
      AllNodeScan [m]
)");
}

TEST(LogicalPlanBuilderTest, BuildsTailFilterAfterApplyPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) WITH DISTINCT n MATCH (m) WHERE n.age > m.age RETURN m",
      R"(ProduceResults [m]
  Projection [m]
    Filter [n.age > m.age]
      Apply
        Distinct [n]
          AllNodeScan [n]
        AllNodeScan [m]
)");
}
