#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "planner/plan_clone.h"
#include "planner/planned_query.h"
#include "runtime/graphdb_planner_catalog.h"
#include "runtime/query_executor.h"
#include "tests/runtime/graphdb_test_utils.h"

namespace {

using Type = ir::LogicalPlanNodeType;

const ir::LogicalPlan *Find(const ir::LogicalPlan &plan, Type type) {
  if (plan.Type() == type) {
    return &plan;
  }
  for (const auto &child : plan.Children()) {
    if (const auto *found = Find(*child, type)) {
      return found;
    }
  }
  return nullptr;
}

std::multiset<std::vector<std::string>> Rows(const rg::QueryResult &result) {
  std::multiset<std::vector<std::string>> rows;
  for (const auto &row : result.rows) {
    std::vector<std::string> values;
    for (const auto &value : row) {
      values.push_back(value.ToString());
    }
    rows.insert(std::move(values));
  }
  return rows;
}

ir::PlannedQuery PlanWithGraphDBCatalog(std::string_view cypher,
                                        graphdb::GraphDB &graph) {
  rg::GraphDBPlannerCatalog catalog(graph);
  ir::LogicalPlanBuilderOptions options;
  options.planner_catalog = &catalog;
  return ir::PlanCypher(cypher, options);
}

}  // namespace

TEST(OpenCypherOptimizerTest, SeeksIdsWithoutScanningAndDeduplicatesValues) {
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({"A"});
  auto b = graph.CreateNode({"B"});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(a, a, "R");
  auto result = rg::test::ExecuteQueryAndCommit(
      graph,
      "MATCH (n) WHERE id(n) IN [1,1.0,2,2,9,-1,null,0.5,'1'] RETURN id(n)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"1"}, {"2"}}));
  result = rg::test::ExecuteQueryAndCommit(
      graph, "MATCH (a)-[r:R]-(b) WHERE id(r) IN [1,1,2,9] RETURN id(a),id(b)");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{
                              {"1", "2"}, {"2", "1"}, {"1", "1"}}));
  EXPECT_TRUE(rg::test::ExecuteQueryAndCommit(
                  graph, "MATCH (n:B) WHERE id(n)=1 RETURN n")
                  .rows.empty());
  EXPECT_TRUE(rg::test::ExecuteQueryAndCommit(
                  graph, "MATCH ()-[r:S]->() WHERE id(r)=1 RETURN r")
                  .rows.empty());
  EXPECT_TRUE(rg::test::ExecuteQueryAndCommit(
                  graph, "MATCH (n) WHERE id(n)=null RETURN n")
                  .rows.empty());
  EXPECT_EQ(rg::test::ExecuteQueryAndCommit(
                graph, "UNWIND [1,2] AS x MATCH (n) WHERE x=id(n) RETURN n")
                .rows.size(),
            2U);
}

TEST(OpenCypherOptimizerTest,
     ProjectsBoundRelationshipsAndListsWithoutRescanning) {
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  auto c = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, c, "R");
  auto result =
      rg::test::ExecuteQueryAndCommit(graph,
                                      "MATCH ()-[r]->() WHERE id(r)=1 WITH r "
                                      "MATCH (a)-[r]-(b) RETURN id(a),id(b)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"1", "2"}, {"2", "1"}}));
  result = rg::test::ExecuteQueryAndCommit(
      graph,
      "MATCH (a)-[rs*2..2]->(b) WHERE id(a)=1 WITH rs "
      "MATCH (x)-[rs*2..2]->(y) RETURN id(x),id(y)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"1", "3"}}));
  result = rg::test::ExecuteQueryAndCommit(
      graph,
      "MATCH (a) WHERE id(a)=1 WITH a, [] AS rs "
      "MATCH (a)-[rs*0..1]->(b) RETURN id(b)");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{{"1"}}));
  EXPECT_TRUE(
      rg::test::ExecuteQueryAndCommit(
          graph,
          "MATCH ()-[r]->() WHERE id(r)=1 WITH r MATCH (a)-[r:S]->(b) RETURN a")
          .rows.empty());
  EXPECT_TRUE(rg::test::ExecuteQueryAndCommit(
                  graph,
                  "MATCH (a) WHERE id(a)=1 OPTIONAL MATCH (a)-[r:S]->() WITH r "
                  "MATCH ()-[r]->() RETURN r")
                  .rows.empty());
}

TEST(OpenCypherOptimizerTest, OptionalExpandKeepsNullsAndDuplicateMatches) {
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(a, b, "R");
  auto result = rg::test::ExecuteQueryAndCommit(
      graph, "MATCH (a) OPTIONAL MATCH (a)-[r:R]->(b) RETURN id(a),id(b)");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{
                              {"1", "2"}, {"1", "2"}, {"2", "null"}}));
  result = rg::test::ExecuteQueryAndCommit(
      graph,
      "MATCH (a) OPTIONAL MATCH (a)-[r:R]->(b) WHERE b.missing=1 "
      "OPTIONAL MATCH (b)-[s:R]->(c) RETURN id(a),b,c");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{
                              {"1", "null", "null"}, {"2", "null", "null"}}));
  result = rg::test::ExecuteQueryAndCommit(
      graph,
      "MATCH (a),(b) OPTIONAL MATCH (a)-[r:R]->(b) RETURN id(a),id(b),id(r)");
  EXPECT_EQ(result.rows.size(), 5U);
  ir::PlannedQuery planned =
      ir::PlanCypher("MATCH (a) OPTIONAL MATCH (a)-[r]->(b) RETURN b");
  EXPECT_NE(Find(planned.Plan(), Type::kOptionalExpand), nullptr);
}

TEST(OpenCypherOptimizerTest, ShortCircuitsExistenceWithoutTraversing) {
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({}, {{"active", rg::Value(true)}});
  auto b = graph.CreateNode({}, {{"active", rg::Value(true)}});
  graph.CreateRelationship(a, b, "R");
  for (const auto &predicate :
       {"a.active OR (a)-[:R]->()", "a.active OR NOT (a)-[:R]->()"}) {
    const std::string text =
        std::string("MATCH (a) WHERE ") + predicate + " RETURN a";
    ir::PlannedQuery query = ir::PlanCypher(text);
    ASSERT_NE(Find(query.Plan(), Type::kSelectOrSemiApply), nullptr);
    EXPECT_EQ(rg::test::ExecutePlanAndCommit(graph, query.Plan()).rows.size(),
              2U);
  }
  EXPECT_EQ(rg::test::ExecuteQueryAndCommit(
                graph, "MATCH (a) WHERE null OR (a)-[:R]->() RETURN a")
                .rows.size(),
            1U);
  EXPECT_EQ(rg::test::ExecuteQueryAndCommit(
                graph, "MATCH (a) WHERE null OR NOT (a)-[:R]->() RETURN a")
                .rows.size(),
            1U);
  EXPECT_TRUE(rg::test::ExecuteQueryAndCommit(
                  graph, "MATCH (a) WHERE null OR (a)-[:S]->() RETURN a")
                  .rows.empty());
}

TEST(OpenCypherOptimizerTest, StreamsVariablePathsAndClosesEarly) {
  rg::test::GraphDBTestDatabase graph;
  auto node = graph.CreateNode({});
  for (int i = 0; i < 100; ++i) {
    auto next = graph.CreateNode({});
    graph.CreateRelationship(node, next, "R");
    node = next;
  }
  rg::QueryOptions options;
  options.execution.memory_limit_bytes = 1024;
  const auto result = rg::test::ExecuteQueryAndCommit(
      graph, "MATCH (a)-[r:R*1..100]->(b) WHERE id(a)=1 RETURN id(b) LIMIT 1",
      options);
  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0][0].AsInteger(), 2);
  auto transaction = graph.BeginTransaction();
  auto cursor = rg::ExecuteQueryCursor(
      *transaction, "MATCH (a)-[r:R*1..100]->(b) WHERE id(a)=1 RETURN b");
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Cancel();
  EXPECT_FALSE(cursor->Next(&row));
}

TEST(OpenCypherOptimizerTest, PruningMatchesTrailEnumerationOnDirectedCycles) {
  const std::array<std::pair<int, int>, 6> edges{
      {{0, 0}, {0, 1}, {1, 0}, {1, 2}, {2, 0}, {2, 2}}};
  for (const auto &bounds : {"0..3", "1..3"}) {
    const std::string prefix = std::string("MATCH (a)-[r:R*") + bounds +
                               "]->(b) WHERE id(a)=1 RETURN ";
    ir::PlannedQuery pruning = ir::PlanCypher(prefix + "DISTINCT id(b)");
    ir::PlannedQuery enumeration = ir::PlanCypher(prefix + "id(b)");
    ASSERT_NE(Find(pruning.Plan(), Type::kPruningVarExpand), nullptr);
    for (unsigned mask = 0; mask < (1U << edges.size()); ++mask) {
      SCOPED_TRACE(mask);
      rg::test::GraphDBTestDatabase graph;
      std::vector<rg::Value::NodePtr> nodes;
      for (int i = 0; i < 3; ++i) {
        nodes.push_back(graph.CreateNode({}));
      }
      for (std::size_t i = 0; i < edges.size(); ++i) {
        if (mask & (1U << i)) {
          graph.CreateRelationship(nodes[edges[i].first],
                                   nodes[edges[i].second], "R");
        }
      }
      auto expected =
          Rows(rg::test::ExecutePlanAndCommit(graph, enumeration.Plan()));
      const std::set<std::vector<std::string>> unique(expected.begin(),
                                                      expected.end());
      const auto actual =
          Rows(rg::test::ExecutePlanAndCommit(graph, pruning.Plan()));
      EXPECT_EQ(actual, (std::multiset<std::vector<std::string>>(
                            unique.begin(), unique.end())));
    }
  }
}

TEST(OpenCypherOptimizerTest, KeepsPathSensitiveQueriesOnRegularExpansion) {
  for (const auto &text :
       {"MATCH (a)-[r:R*1..3]->(b) RETURN DISTINCT r,b",
        "MATCH (a)-[r:R*2..3]->(b) RETURN DISTINCT b",
        "MATCH (a)-[r:R*1..3]-(b) RETURN DISTINCT b",
        "MATCH p=(a)-[r:R*1..3]->(b) RETURN DISTINCT p,b",
        "MATCH (a)-[r:R*1..3]->(b) WHERE size(r)>1 RETURN DISTINCT b",
        "MATCH (a)-[r:R*1..3]->(b) RETURN count(*)",
        "MATCH (a)-[r:R*1..3]->(b) RETURN DISTINCT b,rand()"}) {
    SCOPED_TRACE(text);
    ir::PlannedQuery query = ir::PlanCypher(text);
    EXPECT_EQ(Find(query.Plan(), Type::kPruningVarExpand), nullptr);
  }
}

TEST(OpenCypherOptimizerTest,
     OuterHashJoinPreservesUnmatchedRowsAndDuplicates) {
  ir::PlannedQuery query = ir::PlanCypher(
      "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) RETURN "
      "id(a),id(d),id(c)");
  ASSERT_NE(Find(query.Plan(), Type::kLeftOuterHashJoin), nullptr);
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  auto c = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, c, "S");
  const auto result = rg::test::ExecutePlanAndCommit(graph, query.Plan());
  ASSERT_EQ(result.rows.size(), 12U);
  EXPECT_EQ(std::count_if(result.rows.begin(), result.rows.end(),
                          [](const auto &row) { return row[2].IsNull(); }),
            6);
  ir::PlannedQuery correlated = ir::PlanCypher(
      "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) WHERE c.x=d.x "
      "RETURN c");
  EXPECT_EQ(Find(correlated.Plan(), Type::kLeftOuterHashJoin), nullptr);
  ir::PlannedQuery volatile_query = ir::PlanCypher(
      "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) WHERE rand()>0.5 "
      "RETURN c");
  EXPECT_EQ(Find(volatile_query.Plan(), Type::kLeftOuterHashJoin), nullptr);
}

TEST(OpenCypherOptimizerTest, ClonesNewNodesAndReadsWritesInTheSameQuery) {
  rg::test::GraphDBTestDatabase graph;
  EXPECT_EQ(
      rg::test::ExecuteQueryAndCommit(
          graph,
          "CREATE (a)-[r:R]->(b) WITH r MATCH (x)-[r]->(y) RETURN id(x),id(y)")
          .rows.size(),
      1U);
  EXPECT_EQ(
      rg::test::ExecuteQueryAndCommit(
          graph, "CREATE (n) WITH id(n) AS x MATCH (m) WHERE id(m)=x RETURN m")
          .rows.size(),
      1U);
  for (const auto &text :
       {"MATCH (n) WHERE id(n)=1 RETURN n",
        "MATCH ()-[r]->() WHERE id(r)=1 WITH r MATCH ()-[r]->(b) RETURN b",
        "MATCH (a) OPTIONAL MATCH (a)-[:R]->(b) RETURN a,b",
        "MATCH (a) WHERE true OR (a)-[:R]->() RETURN a",
        "MATCH (a)-[:R*0..2]->(b) RETURN DISTINCT b",
        "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:R]->(c) RETURN c"}) {
    ir::PlannedQuery query = ir::PlanCypher(text);
    auto clone = ir::CloneComponentPlan(query.Plan());
    EXPECT_EQ(Rows(rg::test::ExecutePlanAndCommit(graph, query.Plan())),
              Rows(rg::test::ExecutePlanAndCommit(graph, *clone)));
  }
}

TEST(OpenCypherOptimizerTest,
     PreservesNullableEndpointsAndDeletedRelationships) {
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  EXPECT_TRUE(rg::test::ExecuteQueryAndCommit(
                  graph,
                  "MATCH ()-[r]->() OPTIONAL MATCH (x:Missing) WITH r,x "
                  "MATCH (x)-[r]->(y) RETURN y")
                  .rows.empty());
  EXPECT_TRUE(rg::test::ExecuteQueryAndCommit(
                  graph,
                  "MATCH (a) OPTIONAL MATCH (b:Missing) WITH a,b "
                  "MATCH (a)-[:R*0..2]->(b) RETURN DISTINCT b")
                  .rows.empty());
  EXPECT_TRUE(
      rg::test::ExecuteQueryAndCommit(
          graph,
          "MATCH ()-[r]->() DELETE r WITH r MATCH (a)-[r]->(b) RETURN a,b")
          .rows.empty());
  EXPECT_TRUE(graph.Relationships().empty());
}

TEST(OpenCypherOptimizerTest, PreservesUndirectedListOrientationAndEmptyPaths) {
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, a, "R");
  auto result = rg::test::ExecuteQueryAndCommit(
      graph,
      "MATCH (a)-[rs:R*2..2]->(b) WHERE id(a)=1 WITH rs "
      "MATCH (x)-[rs*2..2]-(y) RETURN id(x),id(y)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"1", "1"}, {"2", "2"}}));
  result = rg::test::ExecuteQueryAndCommit(
      graph, "WITH [] AS rs MATCH (a)-[rs*0..0]->(b) RETURN id(a),id(b)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"1", "1"}, {"2", "2"}}));
  EXPECT_TRUE(rg::test::ExecuteQueryAndCommit(
                  graph, "MATCH (a)-[:R*1..0]->(b) RETURN DISTINCT b")
                  .rows.empty());
  ir::PlannedQuery volatile_optional = ir::PlanCypher(
      "MATCH (a) OPTIONAL MATCH (a)-[r]->(b) WHERE rand()>0.5 RETURN b");
  EXPECT_EQ(Find(volatile_optional.Plan(), Type::kOptionalExpand), nullptr);
}

TEST(OpenCypherOptimizerTest, EnforcesMemoryLimitsForNewStatefulOperators) {
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, a, "S");
  rg::QueryExecutionOptions options;
  options.memory_limit_bytes = 1;
  for (const auto &text :
       {"MATCH (n) WHERE id(n) IN [1,2] RETURN n",
        "MATCH (a)-[r:R*0..2]->(b) RETURN r",
        "MATCH (a)-[r:R*0..2]->(b) RETURN DISTINCT b",
        "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) RETURN c"}) {
    SCOPED_TRACE(text);
    ir::PlannedQuery query = ir::PlanCypher(text);
    EXPECT_THROW(
        (void)rg::test::ExecutePlanAndCommit(graph, query.Plan(), {}, options),
        common::MemoryLimitExceededError);
  }
}

TEST(OpenCypherOptimizerTest, KeepsCorrelatedRelationshipIdSeeksInsideApply) {
  ir::PlannedQuery query = ir::PlanCypher(
      "MATCH (a),(d) OPTIONAL MATCH (a)-[r:R]->(b)-[:S]->(c) "
      "WHERE id(r)=id(a) RETURN id(a),id(d),id(c)");
  EXPECT_NE(Find(query.Plan(), Type::kRelationshipByIdSeek), nullptr);
  EXPECT_EQ(Find(query.Plan(), Type::kLeftOuterHashJoin), nullptr);
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  auto c = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, c, "S");
  const auto result = rg::test::ExecutePlanAndCommit(graph, query.Plan());
  EXPECT_EQ(result.rows.size(), 9U);
  EXPECT_EQ(std::count_if(result.rows.begin(), result.rows.end(),
                          [](const auto &row) { return row[2].IsNull(); }),
            6);
}

TEST(OpenCypherOptimizerTest, KeepsRuntimeNodeAssertionsInOptionalMatches) {
  ir::PlannedQuery query = ir::PlanCypher(
      "UNWIND $values AS a MATCH (d) "
      "OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) RETURN c");
  EXPECT_EQ(Find(query.Plan(), Type::kLeftOuterHashJoin), nullptr);
  rg::test::GraphDBTestDatabase graph;
  graph.CreateNode({});
  rg::QueryParameters parameters{
      {"values", rg::Value(rg::Value::List{rg::Value(1)})}};
  EXPECT_THROW(
      (void)rg::test::ExecutePlanAndCommit(graph, query.Plan(), parameters),
      common::InvalidArgumentError);
}

TEST(OpenCypherOptimizerTest, UsesLabelAndTypeCursorsWithoutFullScans) {
  rg::test::GraphDBTestDatabase graph;
  auto a = graph.CreateNode({"A", "B", "A"});
  auto b = graph.CreateNode({"A"});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(a, a, "R");
  graph.CreateRelationship(a, b, "S");
  for (int i = 0; i < 100; ++i) {
    graph.CreateNode({"Other"});
    graph.CreateRelationship(a, b, "Other");
  }
  auto result =
      rg::test::ExecuteQueryAndCommit(graph, "MATCH (n:A:B) RETURN id(n)");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{{"1"}}));
  ir::PlannedQuery node_scan = ir::PlanCypher("MATCH (n:A:B) RETURN id(n)");
  EXPECT_NE(Find(node_scan.Plan(), Type::kNodeByLabelScan), nullptr);
  EXPECT_TRUE(
      rg::test::ExecuteQueryAndCommit(graph, "MATCH (n:Missing) RETURN n")
          .rows.empty());

  ir::RelationshipTypeScanPlan scan("a", "r", "b", ir::ExpandDirection::kBoth,
                                    {"R", "S", "R", "Missing"});
  result = rg::test::ExecutePlanAndCommit(graph, scan);
  EXPECT_EQ(result.rows.size(), 5U);
}

TEST(OpenCypherOptimizerTest, RangeSeeksOnlyReturnBoundedCandidates) {
  rg::test::GraphDBTestDatabase graph;
  auto endpoint = graph.CreateNode({});
  for (int i = 0; i < 256; ++i) {
    auto node = graph.CreateNode({"N"}, {{"score", rg::Value(i)}});
    graph.CreateRelationship(endpoint, node, "R", {{"score", rg::Value(i)}});
  }
  graph.AddNodeIndex({"N"}, "score");
  graph.AddRelationshipIndex({"R"}, "score");
  rg::QueryOptions options{
      .parameters = {{"lower", rg::Value(100)}, {"upper", rg::Value(103)}}};
  for (const auto &query :
       {"MATCH (n:N) WHERE $lower <= n.score AND n.score > 99 AND "
        "$upper > n.score AND n.score < 200 RETURN n.score"}) {
    SCOPED_TRACE(query);
    auto planned = PlanWithGraphDBCatalog(query, graph.Graph());
    EXPECT_TRUE(Find(planned.Plan(), Type::kNodeIndexRangeSeek) != nullptr ||
                Find(planned.Plan(), Type::kRelationshipIndexRangeSeek) !=
                    nullptr);
    const auto result = rg::test::ExecuteQueryAndCommit(graph, query, options);
    EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{
                                {"100"}, {"101"}, {"102"}}));
  }
  auto relationship_plan = PlanWithGraphDBCatalog(
      "MATCH ()-[r:R]->() WHERE r.score >= $lower AND r.score < $upper "
      "RETURN r.score",
      graph.Graph());
  EXPECT_NE(Find(relationship_plan.Plan(), Type::kRelationshipIndexRangeSeek),
            nullptr);
}

TEST(OpenCypherOptimizerTest, PrefixSeeksUseStringIndexIntervals) {
  rg::test::GraphDBTestDatabase graph;
  auto endpoint = graph.CreateNode({});
  for (const auto *name : {"", "a", "aa", "ab", "abc", "ac", "z"}) {
    auto node = graph.CreateNode({"N"}, {{"name", rg::Value(name)}});
    graph.CreateRelationship(endpoint, node, "R", {{"name", rg::Value(name)}});
  }
  graph.CreateNode({"N"}, {{"name", rg::Value(42)}});
  graph.AddNodeIndex({"N"}, "name");
  graph.AddRelationshipIndex({"R"}, "name");
  rg::QueryOptions options;
  for (const auto &query :
       {"MATCH (n:N) WHERE n.name STARTS WITH $prefix RETURN n.name",
        "MATCH ()-[r:R]->() WHERE r.name STARTS WITH $prefix RETURN r.name"}) {
    for (const auto &prefix :
         {rg::Value("ab"), rg::Value(""), rg::Value("missing"),
          rg::Value::Null(), rg::Value(42)}) {
      SCOPED_TRACE(query);
      SCOPED_TRACE(prefix.ToString());
      options.parameters = {{"prefix", prefix}};
      auto planned = PlanWithGraphDBCatalog(query, graph.Graph());
      EXPECT_TRUE(Find(planned.Plan(), Type::kNodeIndexRangeSeek) != nullptr ||
                  Find(planned.Plan(), Type::kRelationshipIndexRangeSeek) !=
                      nullptr);
      const auto result =
          rg::test::ExecuteQueryAndCommit(graph, query, options);
      const std::size_t expected = prefix.IsString()
                                       ? (prefix.AsString() == "ab"   ? 2U
                                          : prefix.AsString().empty() ? 7U
                                                                      : 0U)
                                       : 0U;
      EXPECT_EQ(result.rows.size(), expected);
    }
  }
}

TEST(OpenCypherOptimizerTest,
     KeepsRowDependentAndVolatileRangeBoundsAsFilters) {
  for (const auto &query : {"MATCH (n:N) WHERE n.x > n.y RETURN n",
                            "MATCH (n:N) WHERE n.x > rand() RETURN n",
                            "MATCH (n:N) WHERE n.x STARTS WITH n.y RETURN n"}) {
    SCOPED_TRACE(query);
    ir::PlannedQuery plan = ir::PlanCypher(query);
    EXPECT_EQ(Find(plan.Plan(), Type::kNodeIndexRangeSeek), nullptr);
    EXPECT_NE(Find(plan.Plan(), Type::kFilter), nullptr);
  }
  rg::test::GraphDBTestDatabase graph;
  graph.CreateNode({"N"}, {{"x", rg::Value(3)}, {"y", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"x", rg::Value(1)}, {"y", rg::Value(2)}});
  graph.AddNodeIndex({"N"}, "x");
  rg::GraphDBPlannerCatalog catalog(graph.Graph());
  rg::QueryOptions options{.planner_catalog = &catalog};
  const auto result = rg::test::ExecuteQueryAndCommit(
      graph, "MATCH (n:N) WHERE n.x >= 0 AND n.x > n.y RETURN n.x", options);
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{{"3"}}));
}
