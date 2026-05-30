#include "ir/logical_plan_builder.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "ast/ast_node.h"
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

std::unique_ptr<ir::LogicalPlan> LogicalPlanFor(
    const std::string &query,
    const ir::LogicalPlanBuilderOptions &options = {}) {
  auto statement = ParseOrFail(query);
  if (!statement) {
    return {};
  }
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  return ir::CreateLogicalPlan(*planner_query, options);
}

void ExpectLogicalPlanText(const std::string &query,
                           const std::string &expected,
                           const ir::LogicalPlanBuilderOptions &options = {}) {
  EXPECT_EQ(LogicalPlanText(query, options), expected);
}

std::unique_ptr<ast::Variable> Variable(std::string name) {
  auto variable = std::make_unique<ast::Variable>();
  variable->name = std::move(name);
  return variable;
}

std::unique_ptr<ast::ComparisonExpression> Equality(
    std::unique_ptr<ast::Expression> left,
    std::unique_ptr<ast::Expression> right) {
  auto expression = std::make_unique<ast::ComparisonExpression>();
  expression->op = "=";
  expression->left = std::move(left);
  expression->right = std::move(right);
  return expression;
}

void ExpectLogicalPlanInvalidArgument(const ir::SinglePlannerQuery &query,
                                      const std::string &message_substring) {
  try {
    (void)ir::CreateLogicalPlan(query);
    FAIL() << "expected InvalidArgumentError";
  } catch (const common::InvalidArgumentError &e) {
    EXPECT_NE(e.Message().find(message_substring), std::string::npos)
        << e.Message();
    EXPECT_EQ(e.Message().find("logical plan"), 0U) << e.Message();
  }
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

TEST(LogicalPlanBuilderTest, AnnotatesCostMetadataAndPrintsWhenRequested) {
  std::unique_ptr<ir::LogicalPlan> plan =
      LogicalPlanFor("MATCH (n:Person) RETURN n");
  ASSERT_NE(plan, nullptr);

  EXPECT_TRUE(plan->EstimatedRows().has_value());
  EXPECT_TRUE(plan->Cost().has_value());
  EXPECT_DOUBLE_EQ(*plan->EstimatedRows(), 100.0);
  EXPECT_DOUBLE_EQ(*plan->Cost(), 102.0);
  EXPECT_DOUBLE_EQ(*plan->Child(0).Cost(), 101.0);
  EXPECT_DOUBLE_EQ(*plan->Child(0).Child(0).Cost(), 100.0);

  EXPECT_EQ(ir::LogicalPlanToString(
                *plan, ir::LogicalPlanPrinterOptions{.include_metadata = true}),
            R"(ProduceResults [n] {rows=100, cost=102}
  Projection [n] {rows=100, cost=101}
    NodeByLabelScan [n:Person] {rows=100, cost=100}
)");
}

TEST(LogicalPlanBuilderTest, UsesMultiLabelPredicateAsLeafScan) {
  ExpectLogicalPlanText("MATCH (n:Person:Employee) RETURN n",
                        R"(ProduceResults [n]
  Projection [n]
    NodeByLabelScan [n:Employee:Person]
)");
}

TEST(LogicalPlanBuilderTest, UsesNodePropertyIndexSeek) {
  ExpectLogicalPlanText("MATCH (n:Person) WHERE n.name = 'Ada' RETURN n",
                        R"(ProduceResults [n]
  Projection [n]
    NodeIndexSeek [n:Person WHERE n.name = 'Ada']
)");
}

TEST(LogicalPlanBuilderTest, UsesNodePropertyIndexRangeSeek) {
  ExpectLogicalPlanText("MATCH (n) WHERE n.age >= 10 AND n.age < 20 RETURN n",
                        R"(ProduceResults [n]
  Projection [n]
    NodeIndexRangeSeek [n WHERE n.age >= 10 AND n.age < 20]
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

TEST(LogicalPlanBuilderTest, UsesIdpPlannerByDefault) {
  ExpectLogicalPlanText("MATCH (a)-[r]->(b:Person) RETURN a, b",
                        R"(ProduceResults [a, b]
  Projection [a, b]
    Expand [(b)<-[r]-(a)]
      NodeByLabelScan [b:Person]
)");
}

TEST(LogicalPlanBuilderTest, IdpPruningKeepsCheapestLeafCandidate) {
  ExpectLogicalPlanText("MATCH (a)-[r]->(b:Person) RETURN a, b",
                        R"(ProduceResults [a, b]
  Projection [a, b]
    Expand [(b)<-[r]-(a)]
      NodeByLabelScan [b:Person]
)",
                        ir::LogicalPlanBuilderOptions{
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
      ir::LogicalPlanBuilderOptions{.planner_statistics = &statistics});
}

TEST(LogicalPlanBuilderTest, IdpUsesRelationshipTypeFanoutStatistics) {
  test_support::FakePlannerStatistics statistics;
  statistics.expand_fanout_by_type = {{"COMMON_REL", 100.0}, {"RARE_REL", 1.0}};
  ExpectLogicalPlanText(
      "MATCH (a)-[r1:COMMON_REL]->(b)-[r2:RARE_REL]->(c) RETURN a, b, c",
      R"(ProduceResults [a, b, c]
  Projection [a, b, c]
    Expand [(b)-[r2:RARE_REL]->(c)]
      RelationshipTypeScan [(a)-[r1:COMMON_REL]->(b)]
)",
      ir::LogicalPlanBuilderOptions{.planner_statistics = &statistics});
}

TEST(LogicalPlanBuilderTest, UsesRelationshipTypeScanWhenCheaper) {
  test_support::FakePlannerStatistics statistics;
  statistics.relationship_count_by_type = {{"RARE_REL", 5.0}};
  ExpectLogicalPlanText(
      "MATCH (a)-[r:RARE_REL]->(b) RETURN r",
      R"(ProduceResults [r]
  Projection [r]
    RelationshipTypeScan [(a)-[r:RARE_REL]->(b)]
)",
      ir::LogicalPlanBuilderOptions{.planner_statistics = &statistics});
}

TEST(LogicalPlanBuilderTest, UsesRelationshipPropertyIndexSeek) {
  ExpectLogicalPlanText("MATCH (a)-[r]->(b) WHERE r.since = 2020 RETURN r",
                        R"(ProduceResults [r]
  Projection [r]
    RelationshipIndexSeek [(a)-[r]->(b) WHERE r.since = 2020]
)");
}

TEST(LogicalPlanBuilderTest, UsesRelationshipPropertyIndexRangeSeek) {
  ExpectLogicalPlanText(
      "MATCH (a)-[r]->(b) WHERE r.since >= 2000 AND r.since < 2020 RETURN r",
      R"(ProduceResults [r]
  Projection [r]
    RelationshipIndexRangeSeek [(a)-[r]->(b) WHERE r.since >= 2000 AND r.since < 2020]
)");
}

TEST(LogicalPlanBuilderTest, PushesRelationshipTypePredicateIntoAccessPath) {
  ExpectLogicalPlanText(
      "MATCH (a)-[r]->(b) WHERE r:KNOWS AND r.since = 2020 RETURN r",
      R"(ProduceResults [r]
  Projection [r]
    RelationshipIndexSeek [(a)-[r:KNOWS]->(b) WHERE r.since = 2020]
)");
}

TEST(LogicalPlanBuilderTest,
     IdpUsesRelationshipTypeExpandIntoSelectivityStatistics) {
  test_support::FakePlannerStatistics statistics;
  statistics.expand_fanout_by_type = {{"AB", 3.0}, {"BC", 3.0}, {"AC", 3.0}};
  statistics.expand_into_selectivity_by_type = {
      {"AB", 0.9}, {"BC", 0.01}, {"AC", 0.9}};
  ExpectLogicalPlanText(
      "MATCH (a)-[r1:AB]->(b), (b)-[r2:BC]->(c), (a)-[r3:AC]->(c) "
      "RETURN a, b, c",
      R"(ProduceResults [a, b, c]
  Projection [a, b, c]
    ExpandInto [(b)-[r2:BC]->(c)]
      Expand [(a)-[r3:AC]->(c)]
        Expand [(a)-[r1:AB]->(b)]
          AllNodeScan [a]
)",
      ir::LogicalPlanBuilderOptions{.planner_statistics = &statistics});
}

TEST(LogicalPlanBuilderTest, IdpBuildsTwoHopJoinFromCheaperMiddleLeaf) {
  ExpectLogicalPlanText("MATCH (a)-[r1]->(b:Person)-[r2]->(c) RETURN a, b, c",
                        R"(ProduceResults [a, b, c]
  Projection [a, b, c]
    Filter [NOT (r1 = r2)]
      NodeHashJoin [b]
        Expand [(b)<-[r1]-(a)]
          NodeByLabelScan [b:Person]
        Expand [(b)-[r2]->(c)]
          NodeByLabelScan [b:Person]
)");
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
)");
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
)");
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
)");
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
)");
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
)");
}

TEST(LogicalPlanBuilderTest, IdpBuildsExpandIntoForBoundEndpoints) {
  ExpectLogicalPlanText("MATCH (a)-[r1]->(b:Person), (a)-[r2]->(b) RETURN a, b",
                        R"(ProduceResults [a, b]
  Projection [a, b]
    Filter [NOT (r1 = r2)]
      ExpandInto [(a)-[r2]->(b)]
        Expand [(b)<-[r1]-(a)]
          NodeByLabelScan [b:Person]
)");
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
    Filter [a:Person]
      Expand [(b)<-[r:KNOWS]-(a)]
        NodeIndexSeek [b WHERE b.name = 'Ada']
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

TEST(LogicalPlanBuilderTest, UsesValueHashJoinForDisconnectedEquality) {
  ExpectLogicalPlanText("MATCH (a), (b) WHERE a.id = b.id RETURN a, b",
                        R"(ProduceResults [a, b]
  Projection [a, b]
    ValueHashJoin [a.id = b.id]
      AllNodeScan [a]
      AllNodeScan [b]
)");
}

TEST(LogicalPlanBuilderTest, UsesPredicateJoinForDisconnectedPredicate) {
  ExpectLogicalPlanText("MATCH (a), (b) WHERE a.age > b.age RETURN a, b",
                        R"(ProduceResults [a, b]
  Projection [a, b]
    PredicateJoin [a.age > b.age]
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

TEST(LogicalPlanBuilderTest, AnnotatesOrderingAndDistinctTraits) {
  std::unique_ptr<ir::LogicalPlan> plan =
      LogicalPlanFor("MATCH (n) RETURN DISTINCT n ORDER BY n");
  ASSERT_NE(plan, nullptr);

  EXPECT_TRUE(plan->DistinctTrait());
  ASSERT_EQ(plan->OrderingTrait().size(), 1U);
  EXPECT_NE(plan->OrderingTrait()[0].expression, nullptr);
  EXPECT_EQ(plan->OrderingTrait()[0].direction,
            ir::LogicalOrderDirection::kAscending);

  const ir::LogicalPlan &sort = plan->Child(0);
  EXPECT_TRUE(sort.DistinctTrait());
  ASSERT_EQ(sort.OrderingTrait().size(), 1U);
  EXPECT_TRUE(sort.Child(0).DistinctTrait());
  EXPECT_TRUE(sort.Child(0).OrderingTrait().empty());
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
)");
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

TEST(LogicalPlanBuilderTest, BuildsProjectionSelectionForWithWhere) {
  ExpectLogicalPlanText("MATCH (n) WITH n AS x WHERE x.age > 30 RETURN x",
                        R"(ProduceResults [x]
  Projection [x]
    Filter [x.age > 30]
      Projection [x]
        AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsUnwindPlan) {
  ExpectLogicalPlanText("UNWIND [1, 2] AS x RETURN x",
                        R"(ProduceResults [x]
  Projection [x]
    Unwind [[1, 2] AS x]
      Argument
)");
}

TEST(LogicalPlanBuilderTest, BuildsStandaloneProcedureCallPlan) {
  ExpectLogicalPlanText("CALL db.labels()",
                        R"(ProcedureCall [CALL db.labels() YIELD label]
  Argument
)");
}

TEST(LogicalPlanBuilderTest, BuildsStandaloneProcedureCallWithYieldWherePlan) {
  ExpectLogicalPlanText("CALL db.labels() YIELD label AS l WHERE l <> ''",
                        R"(Filter [l <> '']
  ProcedureCall [CALL db.labels() YIELD label AS l]
    Argument
)");
}

TEST(LogicalPlanBuilderTest, BuildsInQueryProcedureCallPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) CALL db.labels() YIELD label AS l WHERE l <> '' RETURN n, l",
      R"(ProduceResults [n, l]
  Projection [n, l]
    Filter [l <> '']
      ProcedureCall [CALL db.labels() YIELD label AS l]
        AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsProcedureCallWithArgumentDependencyPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) CALL db.unknown(n.name) YIELD value RETURN n, value",
      R"(ProduceResults [n, value]
  Projection [n, value]
    ProcedureCall [CALL db.unknown(n.name) YIELD value]
      WriteBarrier
        AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, AnnotatesProcedureCallCostMetadata) {
  test_support::FakePlannerStatistics statistics;
  statistics.procedure_rows = 3.0;
  std::unique_ptr<ir::LogicalPlan> plan = LogicalPlanFor(
      "CALL db.labels()",
      ir::LogicalPlanBuilderOptions{.planner_statistics = &statistics});
  ASSERT_NE(plan, nullptr);

  EXPECT_EQ(plan->Type(), ir::LogicalPlanNodeType::kProcedureCall);
  EXPECT_DOUBLE_EQ(*plan->EstimatedRows(), 3.0);
  EXPECT_DOUBLE_EQ(*plan->Cost(), 3.25);
  EXPECT_DOUBLE_EQ(*plan->Child(0).Cost(), 0.1);

  EXPECT_EQ(ir::LogicalPlanToString(
                *plan, ir::LogicalPlanPrinterOptions{.include_metadata = true}),
            R"(ProcedureCall [CALL db.labels() YIELD label] {rows=3, cost=3.25}
  Argument {rows=1, cost=0.1}
)");
}

TEST(LogicalPlanBuilderTest, BuildsUnionAllPlan) {
  ExpectLogicalPlanText("RETURN 1 AS a UNION ALL RETURN 2 AS a",
                        R"(ProduceResults [a]
  Union [ALL a]
    Projection [a]
      Argument
    Projection [a]
      Argument
)");
}

TEST(LogicalPlanBuilderTest, BuildsUnionDistinctPlan) {
  ExpectLogicalPlanText("MATCH (n) RETURN n AS x UNION MATCH (m) RETURN m AS x",
                        R"(ProduceResults [x]
  Union [DISTINCT x]
    Projection [x]
      AllNodeScan [n]
    Projection [x]
      AllNodeScan [m]
)");
}

TEST(LogicalPlanBuilderTest, BuildsExistsSubquerySemiApplyPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) WHERE EXISTS { MATCH (n)-[r]->(m) RETURN 1 AS ok } RETURN n",
      R"(ProduceResults [n]
  Projection [n]
    SemiApply
      AllNodeScan [n]
      Projection [ok]
        Expand [(n)-[r]->(m)]
          Argument [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsNotExistsSubqueryAntiSemiApplyPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) WHERE NOT EXISTS { MATCH (n)-[r]->(m) RETURN 1 AS ok } "
      "RETURN n",
      R"(ProduceResults [n]
  Projection [n]
    AntiSemiApply
      AllNodeScan [n]
      Projection [ok]
        Expand [(n)-[r]->(m)]
          Argument [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsWithWhereExistsSubquerySemiApplyPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) WITH n AS x "
      "WHERE EXISTS { MATCH (x)-[r]->(m) RETURN 1 AS ok } RETURN x",
      R"(ProduceResults [x]
  Projection [x]
    SemiApply
      Projection [x]
        AllNodeScan [n]
      Projection [ok]
        Expand [(x)-[r]->(m)]
          Argument [x]
)");
}

TEST(LogicalPlanBuilderTest, BuildsProjectionExistsLetSemiApplyPlan) {
  ExpectLogicalPlanText(
      "MATCH (n) RETURN EXISTS { MATCH (n)-[r]->(m) RETURN 1 AS ok } AS has",
      R"(ProduceResults [has]
  Projection [has]
    LetSemiApply [__exists_0]
      AllNodeScan [n]
      Projection [ok]
        Expand [(n)-[r]->(m)]
          Argument [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsPatternComprehensionRollUpApplyPlan) {
  ExpectLogicalPlanText("MATCH (n) RETURN [(n)-[r]->(m) | m.name] AS names",
                        R"(ProduceResults [names]
  Projection [names]
    RollUpApply [__list_1 <- __list_value_0]
      AllNodeScan [n]
      Projection [__list_value_0]
        Expand [(n)-[r]->(m)]
          Argument [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsOptionalApplyPlan) {
  ExpectLogicalPlanText("MATCH (a) OPTIONAL MATCH (a)-[r]->(b) RETURN a, b",
                        R"(ProduceResults [a, b]
  Projection [a, b]
    OptionalApply
      AllNodeScan [a]
      Expand [(a)-[r]->(b)]
        Argument [a]
)");
}

TEST(LogicalPlanBuilderTest, BuildsSequentialOptionalApplyPlans) {
  ExpectLogicalPlanText(
      "MATCH (a) OPTIONAL MATCH (a)-[r]->(b) "
      "OPTIONAL MATCH (b)-[s]->(c) RETURN c",
      R"(ProduceResults [c]
  Projection [c]
    OptionalApply
      OptionalApply
        AllNodeScan [a]
        Expand [(a)-[r]->(b)]
          Argument [a]
      Expand [(b)-[s]->(c)]
        Argument [b]
)");
}

TEST(LogicalPlanBuilderTest, BuildsOptionalMatchWithLocalWherePlan) {
  ExpectLogicalPlanText(
      "MATCH (a) OPTIONAL MATCH (a)-[r]->(b) WHERE b.age > 1 RETURN b",
      R"(ProduceResults [b]
  Projection [b]
    OptionalApply
      AllNodeScan [a]
      Filter [b.age > 1]
        Expand [(a)-[r]->(b)]
          Argument [a]
)");
}

TEST(LogicalPlanBuilderTest, BuildsVariableLengthExpandPlan) {
  ExpectLogicalPlanText("MATCH (a)-[r*1..3]->(b) RETURN r",
                        R"(ProduceResults [r]
  Projection [r]
    Filter [ALL(__uniq_rel_0 IN r WHERE SINGLE(__uniq_rel_1 IN r WHERE __uniq_rel_0 = __uniq_rel_1))]
      VarExpand [(a)-[r*1..3]->(b)]
        AllNodeScan [a]
)");
}

TEST(LogicalPlanBuilderTest, BuildsVariableLengthExpandBetweenArgumentsPlan) {
  ExpectLogicalPlanText(
      "MATCH (a), (b) WITH DISTINCT a, b MATCH (a)-[r*0..2]->(b) RETURN r",
      R"(ProduceResults [r]
  Projection [r]
    Apply
      Distinct [a, b]
        CartesianProduct
          AllNodeScan [a]
          AllNodeScan [b]
      Filter [ALL(__uniq_rel_0 IN r WHERE SINGLE(__uniq_rel_1 IN r WHERE __uniq_rel_0 = __uniq_rel_1))]
        VarExpand [(a)-[r*0..2]->(b)]
          Argument [a, b]
)");
}

TEST(LogicalPlanBuilderTest, BuildsNamedPathPlan) {
  ExpectLogicalPlanText("MATCH p = (a)-[r]->(b) RETURN p",
                        R"(ProduceResults [p]
  Projection [p]
    PathBuild [p]
      Expand [(a)-[r]->(b)]
        AllNodeScan [a]
)");
}

TEST(LogicalPlanBuilderTest, BuildsNamedPathFilterAfterPathBuildPlan) {
  ExpectLogicalPlanText("MATCH p = (a)-[r]->(b) WHERE length(p) > 0 RETURN p",
                        R"(ProduceResults [p]
  Projection [p]
    Filter [length(p) > 0]
      PathBuild [p]
        Expand [(a)-[r]->(b)]
          AllNodeScan [a]
)");
}

TEST(LogicalPlanBuilderTest, BuildsArgumentNodeAssertionPlan) {
  ExpectLogicalPlanText("MATCH (n) WITH n, 1 AS keep MATCH (n) RETURN n",
                        R"(ProduceResults [n]
  Projection [n]
    Apply
      Projection [n, keep]
        AllNodeScan [n]
      AssertIsNode [n]
        Argument [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsCreatePlan) {
  ExpectLogicalPlanText(
      "CREATE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2024}]->(b) "
      "RETURN a, r, b",
      R"(ProduceResults [a, r, b]
  Projection [a, r, b]
    CreateRelationship [(a)-[r:KNOWS {since: 2024}]->(b)]
      CreateNode [b]
        CreateNode [a:Person {name: 'Ada'}]
          WriteBarrier
            Argument
)");
}

TEST(LogicalPlanBuilderTest, BuildsCreateWithBoundEndpointPlan) {
  ExpectLogicalPlanText("MATCH (a) CREATE (a)-[r:KNOWS]->(b) RETURN r",
                        R"(ProduceResults [r]
  Projection [r]
    CreateRelationship [(a)-[r:KNOWS]->(b)]
      CreateNode [b]
        WriteBarrier
          AllNodeScan [a]
)");
}

TEST(LogicalPlanBuilderTest, BuildsNamedCreatePathPlan) {
  ExpectLogicalPlanText("CREATE p = (a)-[r]->(b) RETURN p",
                        R"(ProduceResults [p]
  Projection [p]
    PathBuild [p]
      CreateRelationship [(a)-[r]->(b)]
        CreateNode [b]
          CreateNode [a]
            WriteBarrier
              Argument
)");
}

TEST(LogicalPlanBuilderTest, BuildsSetRemoveAndDeletePlans) {
  ExpectLogicalPlanText(
      "MATCH (n) SET n.name = 'Ada', n = {age: 42}, n += {score: 7}, "
      "n:New REMOVE n:Old, n.name DELETE n",
      R"(Delete [n]
  RemoveProperty [n.name]
    RemoveLabels [n:Old]
      SetLabels [n:New]
        SetProperties [n += {score: 7}]
          SetProperties [n = {age: 42}]
            SetProperty [n.name = 'Ada']
              WriteBarrier
                AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsDetachDeletePlan) {
  ExpectLogicalPlanText("MATCH (n) DETACH DELETE n",
                        R"(DetachDelete [n]
  WriteBarrier
    AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsSetWithReturnPlan) {
  ExpectLogicalPlanText("MATCH (n) SET n.name = 'Ada' RETURN n",
                        R"(ProduceResults [n]
  Projection [n]
    SetProperty [n.name = 'Ada']
      WriteBarrier
        AllNodeScan [n]
)");
}

TEST(LogicalPlanBuilderTest, BuildsMergePlan) {
  ExpectLogicalPlanText(
      "MATCH (m) MERGE (m)-[r:KNOWS {since: 2020}]->"
      "(n:Person {id: m.id}) ON MATCH SET r.seen = true RETURN r",
      R"(ProduceResults [r]
  Projection [r]
    Apply
      AllNodeScan [m]
      Merge [(m)-[r:KNOWS {since: 2020}]->(n:Person {id: m.id}) ON MATCH SET r.seen = true]
        WriteBarrier
          Argument [m]
        Expand [(m)-[r:KNOWS]->(n)]
          Argument [m]
)");
}

TEST(LogicalPlanBuilderTest, RejectsPlannerHintsWithLogicalPlanStage) {
  ir::SinglePlannerQuery query;
  query.query_graph.hints.push_back(ir::Hint{});

  ExpectLogicalPlanInvalidArgument(query, "planner hint is not supported");
}

TEST(LogicalPlanBuilderTest,
     RejectsTailArgumentMissingFromInputWithLogicalPlanStage) {
  ir::SinglePlannerQuery query;
  query.query_graph.pattern_nodes.insert("n");
  query.horizon = ir::QueryHorizon::ForPassthrough();
  query.tail = std::make_unique<ir::SinglePlannerQuery>();
  query.tail->query_graph.argument_ids.insert("missing");

  ExpectLogicalPlanInvalidArgument(query,
                                   "tail argument is not available: missing");
}

TEST(LogicalPlanBuilderTest,
     RejectsProcedureArgumentUnmetDependenciesWithLogicalPlanStage) {
  ir::SinglePlannerQuery query;
  ir::ProcedureCallHorizon call;
  call.procedure_name = "db.unknown";
  std::unique_ptr<ast::Variable> missing = Variable("missing");
  call.arguments.push_back(missing.get());
  call.yield_items.push_back({.variable = "out"});
  query.horizon = ir::QueryHorizon::ForProcedureCall(std::move(call));

  ExpectLogicalPlanInvalidArgument(
      query, "procedure argument with unmet dependencies is not supported");
}

TEST(LogicalPlanBuilderTest,
     RejectsSelectionPredicateUnmetDependenciesWithLogicalPlanStage) {
  ir::SinglePlannerQuery query;
  query.query_graph.pattern_nodes.insert("n");
  auto predicate_expression = Equality(Variable("n"), Variable("missing"));
  const ast::Expression *predicate_ptr = predicate_expression.get();
  query.query_graph.selections.predicates.push_back(
      {.expression = predicate_ptr,
       .dependencies = {"n", "missing"},
       .kind = ir::PredicateKind::kGenericExpression});

  ExpectLogicalPlanInvalidArgument(
      query, "selection predicate with unmet dependencies is not supported");
}
