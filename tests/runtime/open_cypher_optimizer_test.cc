#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
#include "common/exception.h"
#include "ir/query_ir.h"
#include "planner/logical_plan_builder.h"
#include "planner/plan_clone.h"
#include "runtime/query_executor.h"
#include "storage/in_memory_graph.h"

namespace {

using Type = ir::LogicalPlanNodeType;

struct Query {
  std::unique_ptr<ast::Statement> statement;
  std::unique_ptr<ir::QueryIR> ir;
  std::unique_ptr<ir::LogicalPlan> plan;

  explicit Query(const std::string &text) {
    statement = ast::ParseCypherAndRewrite(text);
    ir = ir::CreateQueryIR(*statement);
    plan = ir::CreateLogicalPlan(*ir);
  }
};

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

class ObservedGraph final : public rg::GraphReader {
 public:
  rg::InMemoryGraph graph;
  mutable std::size_t scans = 0;
  mutable std::size_t expansions = 0;
  mutable std::size_t label_scans = 0;
  mutable std::size_t type_scans = 0;
  mutable std::size_t range_seeks = 0;
  mutable std::size_t candidates = 0;

  class CountingCursor final : public rg::EntityIdCursor {
   public:
    CountingCursor(std::unique_ptr<rg::EntityIdCursor> source,
                   std::size_t *count)
        : source_(std::move(source)), count_(count) {}
    bool Next() override {
      if (!source_->Next()) {
        return false;
      }
      ++*count_;
      return true;
    }
    std::int64_t Id() const override { return source_->Id(); }
    void Close() noexcept override { source_->Close(); }

   private:
    std::unique_ptr<rg::EntityIdCursor> source_;
    std::size_t *count_;
  };

  std::unique_ptr<rg::EntityIdCursor> Count(
      std::unique_ptr<rg::EntityIdCursor> source) const {
    return std::make_unique<CountingCursor>(std::move(source), &candidates);
  }

  std::unique_ptr<rg::EntityIdCursor> ScanNodeIds() const override {
    ++scans;
    return graph.ScanNodeIds();
  }
  std::unique_ptr<rg::EntityIdCursor> ScanRelationshipIds() const override {
    ++scans;
    return graph.ScanRelationshipIds();
  }
  std::unique_ptr<rg::EntityIdCursor> ScanNodeIdsByLabels(
      const std::vector<std::string> &labels) const override {
    ++label_scans;
    return Count(graph.ScanNodeIdsByLabels(labels));
  }
  std::unique_ptr<rg::EntityIdCursor> ScanRelationshipIdsByTypes(
      const std::vector<std::string> &types) const override {
    ++type_scans;
    return Count(graph.ScanRelationshipIdsByTypes(types));
  }
  std::unique_ptr<rg::EntityIdCursor> RelationshipIdsConnectedTo(
      std::int64_t id) const override {
    ++expansions;
    return graph.RelationshipIdsConnectedTo(id);
  }
  std::unique_ptr<rg::EntityIdCursor> OutgoingRelationshipIds(
      std::int64_t id) const override {
    ++expansions;
    return graph.OutgoingRelationshipIds(id);
  }
  std::unique_ptr<rg::EntityIdCursor> IncomingRelationshipIds(
      std::int64_t id) const override {
    ++expansions;
    return graph.IncomingRelationshipIds(id);
  }
  std::unique_ptr<rg::EntityIdCursor> FindNodeIdsByIndex(
      const std::vector<std::string> &labels, std::string_view key,
      const rg::Value &value) const override {
    return graph.FindNodeIdsByIndex(labels, key, value);
  }
  std::unique_ptr<rg::EntityIdCursor> FindNodeIdsByIndexRange(
      const std::vector<std::string> &labels, std::string_view key,
      const rg::IndexRange &range) const override {
    ++range_seeks;
    return Count(graph.FindNodeIdsByIndexRange(labels, key, range));
  }
  std::unique_ptr<rg::EntityIdCursor> FindRelationshipIdsByIndex(
      const std::vector<std::string> &types, std::string_view key,
      const rg::Value &value) const override {
    return graph.FindRelationshipIdsByIndex(types, key, value);
  }
  std::unique_ptr<rg::EntityIdCursor> FindRelationshipIdsByIndexRange(
      const std::vector<std::string> &types, std::string_view key,
      const rg::IndexRange &range) const override {
    ++range_seeks;
    return Count(graph.FindRelationshipIdsByIndexRange(types, key, range));
  }
  std::size_t RelationshipCount() const override {
    return graph.RelationshipCount();
  }
  const NodePtr &NodeById(std::int64_t id) const override {
    return graph.NodeById(id);
  }
  const RelationshipPtr &RelationshipById(std::int64_t id) const override {
    return graph.RelationshipById(id);
  }
  rg::Value NodeProperty(std::int64_t id, std::string_view key) const override {
    return graph.NodeProperty(id, key);
  }
  rg::Value RelationshipProperty(std::int64_t id,
                                 std::string_view key) const override {
    return graph.RelationshipProperty(id, key);
  }
};

}  // namespace

TEST(OpenCypherOptimizerTest, SeeksIdsWithoutScanningAndDeduplicatesValues) {
  ObservedGraph graph;
  auto a = graph.graph.CreateNode({"A"});
  auto b = graph.graph.CreateNode({"B"});
  graph.graph.CreateRelationship(a, b, "R");
  graph.graph.CreateRelationship(a, a, "R");
  auto result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n) WHERE id(n) IN [0,0.0,1,1,9,-1,null,0.5,'0'] RETURN id(n)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"0"}, {"1"}}));
  result = rg::ExecuteReadQuery(
      graph, "MATCH (a)-[r:R]-(b) WHERE id(r) IN [0,0,1,9] RETURN id(a),id(b)");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{
                              {"0", "1"}, {"1", "0"}, {"0", "0"}}));
  EXPECT_EQ(graph.scans, 0U);
  EXPECT_EQ(graph.expansions, 0U);
  EXPECT_TRUE(rg::ExecuteReadQuery(graph, "MATCH (n:B) WHERE id(n)=0 RETURN n")
                  .rows.empty());
  EXPECT_TRUE(
      rg::ExecuteReadQuery(graph, "MATCH ()-[r:S]->() WHERE id(r)=0 RETURN r")
          .rows.empty());
  EXPECT_TRUE(rg::ExecuteReadQuery(graph, "MATCH (n) WHERE id(n)=null RETURN n")
                  .rows.empty());
  EXPECT_EQ(rg::ExecuteReadQuery(
                graph, "UNWIND [0,1] AS x MATCH (n) WHERE x=id(n) RETURN n")
                .rows.size(),
            2U);
}

TEST(OpenCypherOptimizerTest,
     ProjectsBoundRelationshipsAndListsWithoutRescanning) {
  ObservedGraph graph;
  auto a = graph.graph.CreateNode({});
  auto b = graph.graph.CreateNode({});
  auto c = graph.graph.CreateNode({});
  graph.graph.CreateRelationship(a, b, "R");
  graph.graph.CreateRelationship(b, c, "R");
  auto result = rg::ExecuteReadQuery(graph,
                                     "MATCH ()-[r]->() WHERE id(r)=0 WITH r "
                                     "MATCH (a)-[r]-(b) RETURN id(a),id(b)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"0", "1"}, {"1", "0"}}));
  EXPECT_EQ(graph.scans, 0U);
  EXPECT_EQ(graph.expansions, 0U);
  result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (a)-[rs*2..2]->(b) WHERE id(a)=0 WITH rs "
                           "MATCH (x)-[rs*2..2]->(y) RETURN id(x),id(y)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"0", "2"}}));
  EXPECT_EQ(graph.scans, 0U);
  EXPECT_EQ(graph.expansions, 2U);
  result = rg::ExecuteReadQuery(graph,
                                "MATCH (a) WHERE id(a)=0 WITH a, [] AS rs "
                                "MATCH (a)-[rs*0..1]->(b) RETURN id(b)");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{{"0"}}));
  EXPECT_TRUE(
      rg::ExecuteReadQuery(
          graph,
          "MATCH ()-[r]->() WHERE id(r)=0 WITH r MATCH (a)-[r:S]->(b) RETURN a")
          .rows.empty());
  EXPECT_TRUE(rg::ExecuteReadQuery(
                  graph,
                  "MATCH (a) WHERE id(a)=0 OPTIONAL MATCH (a)-[r:S]->() WITH r "
                  "MATCH ()-[r]->() RETURN r")
                  .rows.empty());
}

TEST(OpenCypherOptimizerTest, OptionalExpandKeepsNullsAndDuplicateMatches) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(a, b, "R");
  auto result = rg::ExecuteReadQuery(
      graph, "MATCH (a) OPTIONAL MATCH (a)-[r:R]->(b) RETURN id(a),id(b)");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{
                              {"0", "1"}, {"0", "1"}, {"1", "null"}}));
  result = rg::ExecuteReadQuery(
      graph,
      "MATCH (a) OPTIONAL MATCH (a)-[r:R]->(b) WHERE b.missing=1 "
      "OPTIONAL MATCH (b)-[s:R]->(c) RETURN id(a),b,c");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{
                              {"0", "null", "null"}, {"1", "null", "null"}}));
  result = rg::ExecuteReadQuery(
      graph,
      "MATCH (a),(b) OPTIONAL MATCH (a)-[r:R]->(b) RETURN id(a),id(b),id(r)");
  EXPECT_EQ(result.rows.size(), 5U);
  Query planned("MATCH (a) OPTIONAL MATCH (a)-[r]->(b) RETURN b");
  EXPECT_NE(Find(*planned.plan, Type::kOptionalExpand), nullptr);
}

TEST(OpenCypherOptimizerTest, ShortCircuitsExistenceWithoutTraversing) {
  ObservedGraph graph;
  auto a = graph.graph.CreateNode({}, {{"active", rg::Value(true)}});
  auto b = graph.graph.CreateNode({}, {{"active", rg::Value(true)}});
  graph.graph.CreateRelationship(a, b, "R");
  for (const auto &predicate :
       {"a.active OR (a)-[:R]->()", "a.active OR NOT (a)-[:R]->()"}) {
    const std::string text =
        std::string("MATCH (a) WHERE ") + predicate + " RETURN a";
    Query query(text);
    ASSERT_NE(Find(*query.plan, Type::kSelectOrSemiApply), nullptr);
    EXPECT_EQ(rg::QueryExecutor(graph).Execute(*query.plan).rows.size(), 2U);
  }
  EXPECT_EQ(graph.expansions, 0U);
  EXPECT_EQ(rg::ExecuteReadQuery(
                graph, "MATCH (a) WHERE null OR (a)-[:R]->() RETURN a")
                .rows.size(),
            1U);
  EXPECT_EQ(rg::ExecuteReadQuery(
                graph, "MATCH (a) WHERE null OR NOT (a)-[:R]->() RETURN a")
                .rows.size(),
            1U);
  EXPECT_TRUE(rg::ExecuteReadQuery(
                  graph, "MATCH (a) WHERE null OR (a)-[:S]->() RETURN a")
                  .rows.empty());
}

TEST(OpenCypherOptimizerTest, StreamsVariablePathsAndClosesEarly) {
  ObservedGraph graph;
  auto node = graph.graph.CreateNode({});
  for (int i = 0; i < 100; ++i) {
    auto next = graph.graph.CreateNode({});
    graph.graph.CreateRelationship(node, next, "R");
    node = next;
  }
  rg::QueryOptions options;
  options.execution.memory_limit_bytes = 1024;
  const auto result = rg::ExecuteReadQuery(
      graph, "MATCH (a)-[r:R*1..100]->(b) WHERE id(a)=0 RETURN id(b) LIMIT 1",
      options);
  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0][0].AsInteger(), 1);
  EXPECT_EQ(graph.expansions, 1U);
  auto cursor = rg::ExecuteReadQueryCursor(
      graph, "MATCH (a)-[r:R*1..100]->(b) WHERE id(a)=0 RETURN b");
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
                               "]->(b) WHERE id(a)=0 RETURN ";
    Query pruning(prefix + "DISTINCT id(b)");
    Query enumeration(prefix + "id(b)");
    ASSERT_NE(Find(*pruning.plan, Type::kPruningVarExpand), nullptr);
    for (unsigned mask = 0; mask < (1U << edges.size()); ++mask) {
      SCOPED_TRACE(mask);
      rg::InMemoryGraph graph;
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
      auto expected = Rows(rg::QueryExecutor(graph).Execute(*enumeration.plan));
      const std::set<std::vector<std::string>> unique(expected.begin(),
                                                      expected.end());
      const auto actual = Rows(rg::QueryExecutor(graph).Execute(*pruning.plan));
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
    Query query(text);
    EXPECT_EQ(Find(*query.plan, Type::kPruningVarExpand), nullptr);
  }
}

TEST(OpenCypherOptimizerTest,
     OuterHashJoinPreservesUnmatchedRowsAndDuplicates) {
  Query query(
      "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) RETURN "
      "id(a),id(d),id(c)");
  ASSERT_NE(Find(*query.plan, Type::kLeftOuterHashJoin), nullptr);
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  auto c = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, c, "S");
  const auto result = rg::QueryExecutor(graph).Execute(*query.plan);
  ASSERT_EQ(result.rows.size(), 12U);
  EXPECT_EQ(std::count_if(result.rows.begin(), result.rows.end(),
                          [](const auto &row) { return row[2].IsNull(); }),
            6);
  Query correlated(
      "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) WHERE c.x=d.x "
      "RETURN c");
  EXPECT_EQ(Find(*correlated.plan, Type::kLeftOuterHashJoin), nullptr);
  Query volatile_query(
      "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) WHERE rand()>0.5 "
      "RETURN c");
  EXPECT_EQ(Find(*volatile_query.plan, Type::kLeftOuterHashJoin), nullptr);
}

TEST(OpenCypherOptimizerTest, ClonesNewNodesAndReadsWritesInTheSameQuery) {
  rg::InMemoryGraph graph;
  EXPECT_EQ(
      rg::ExecuteQuery(
          graph,
          "CREATE (a)-[r:R]->(b) WITH r MATCH (x)-[r]->(y) RETURN id(x),id(y)")
          .rows.size(),
      1U);
  EXPECT_EQ(
      rg::ExecuteQuery(
          graph, "CREATE (n) WITH id(n) AS x MATCH (m) WHERE id(m)=x RETURN m")
          .rows.size(),
      1U);
  for (const auto &text :
       {"MATCH (n) WHERE id(n)=0 RETURN n",
        "MATCH ()-[r]->() WHERE id(r)=0 WITH r MATCH ()-[r]->(b) RETURN b",
        "MATCH (a) OPTIONAL MATCH (a)-[:R]->(b) RETURN a,b",
        "MATCH (a) WHERE true OR (a)-[:R]->() RETURN a",
        "MATCH (a)-[:R*0..2]->(b) RETURN DISTINCT b",
        "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:R]->(c) RETURN c"}) {
    Query query(text);
    auto clone = ir::CloneComponentPlan(*query.plan);
    EXPECT_EQ(Rows(rg::QueryExecutor(graph).Execute(*query.plan)),
              Rows(rg::QueryExecutor(graph).Execute(*clone)));
  }
}

TEST(OpenCypherOptimizerTest,
     PreservesNullableEndpointsAndDeletedRelationships) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  EXPECT_TRUE(rg::ExecuteReadQuery(
                  graph,
                  "MATCH ()-[r]->() OPTIONAL MATCH (x:Missing) WITH r,x "
                  "MATCH (x)-[r]->(y) RETURN y")
                  .rows.empty());
  EXPECT_TRUE(
      rg::ExecuteReadQuery(graph,
                           "MATCH (a) OPTIONAL MATCH (b:Missing) WITH a,b "
                           "MATCH (a)-[:R*0..2]->(b) RETURN DISTINCT b")
          .rows.empty());
  EXPECT_TRUE(
      rg::ExecuteQuery(
          graph,
          "MATCH ()-[r]->() DELETE r WITH r MATCH (a)-[r]->(b) RETURN a,b")
          .rows.empty());
  EXPECT_TRUE(graph.Relationships().empty());
}

TEST(OpenCypherOptimizerTest, PreservesUndirectedListOrientationAndEmptyPaths) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, a, "R");
  auto result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (a)-[rs:R*2..2]->(b) WHERE id(a)=0 WITH rs "
                           "MATCH (x)-[rs*2..2]-(y) RETURN id(x),id(y)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"0", "0"}, {"1", "1"}}));
  result = rg::ExecuteReadQuery(
      graph, "WITH [] AS rs MATCH (a)-[rs*0..0]->(b) RETURN id(a),id(b)");
  EXPECT_EQ(Rows(result),
            (std::multiset<std::vector<std::string>>{{"0", "0"}, {"1", "1"}}));
  EXPECT_TRUE(
      rg::ExecuteReadQuery(graph, "MATCH (a)-[:R*1..0]->(b) RETURN DISTINCT b")
          .rows.empty());
  Query volatile_optional(
      "MATCH (a) OPTIONAL MATCH (a)-[r]->(b) WHERE rand()>0.5 RETURN b");
  EXPECT_EQ(Find(*volatile_optional.plan, Type::kOptionalExpand), nullptr);
}

TEST(OpenCypherOptimizerTest, EnforcesMemoryLimitsForNewStatefulOperators) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, a, "S");
  rg::QueryExecutionOptions options;
  options.memory_limit_bytes = 1;
  for (const auto &text :
       {"MATCH (n) WHERE id(n) IN [0,1] RETURN n",
        "MATCH (a)-[r:R*0..2]->(b) RETURN r",
        "MATCH (a)-[r:R*0..2]->(b) RETURN DISTINCT b",
        "MATCH (a),(d) OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) RETURN c"}) {
    SCOPED_TRACE(text);
    Query query(text);
    EXPECT_THROW(
        (void)rg::QueryExecutor(graph).Execute(*query.plan, {}, options),
        common::MemoryLimitExceededError);
  }
}

TEST(OpenCypherOptimizerTest, KeepsCorrelatedRelationshipIdSeeksInsideApply) {
  Query query(
      "MATCH (a),(d) OPTIONAL MATCH (a)-[r:R]->(b)-[:S]->(c) "
      "WHERE id(r)=id(a) RETURN id(a),id(d),id(c)");
  EXPECT_NE(Find(*query.plan, Type::kRelationshipByIdSeek), nullptr);
  EXPECT_EQ(Find(*query.plan, Type::kLeftOuterHashJoin), nullptr);
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  auto c = graph.CreateNode({});
  graph.CreateRelationship(a, b, "R");
  graph.CreateRelationship(b, c, "S");
  const auto result = rg::QueryExecutor(graph).Execute(*query.plan);
  EXPECT_EQ(result.rows.size(), 9U);
  EXPECT_EQ(std::count_if(result.rows.begin(), result.rows.end(),
                          [](const auto &row) { return row[2].IsNull(); }),
            6);
}

TEST(OpenCypherOptimizerTest, KeepsRuntimeNodeAssertionsInOptionalMatches) {
  Query query(
      "UNWIND $values AS a MATCH (d) "
      "OPTIONAL MATCH (a)-[:R]->(b)-[:S]->(c) RETURN c");
  EXPECT_EQ(Find(*query.plan, Type::kLeftOuterHashJoin), nullptr);
  rg::InMemoryGraph graph;
  graph.CreateNode({});
  rg::QueryParameters parameters{
      {"values", rg::Value(rg::Value::List{rg::Value(1)})}};
  EXPECT_THROW((void)rg::QueryExecutor(graph).Execute(*query.plan, parameters),
               common::InvalidArgumentError);
}

TEST(OpenCypherOptimizerTest, UsesLabelAndTypeCursorsWithoutFullScans) {
  ObservedGraph graph;
  auto a = graph.graph.CreateNode({"A", "B", "A"});
  auto b = graph.graph.CreateNode({"A"});
  graph.graph.CreateRelationship(a, b, "R");
  graph.graph.CreateRelationship(a, a, "R");
  graph.graph.CreateRelationship(a, b, "S");
  for (int i = 0; i < 100; ++i) {
    graph.graph.CreateNode({"Other"});
    graph.graph.CreateRelationship(a, b, "Other");
  }
  auto result = rg::ExecuteReadQuery(graph, "MATCH (n:A:B) RETURN id(n)");
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{{"0"}}));
  EXPECT_EQ(graph.label_scans, 1U);
  EXPECT_EQ(graph.candidates, 1U);
  EXPECT_TRUE(
      rg::ExecuteReadQuery(graph, "MATCH (n:Missing) RETURN n").rows.empty());

  graph.candidates = 0;
  ir::RelationshipTypeScanPlan scan("a", "r", "b", ir::ExpandDirection::kBoth,
                                    {"R", "S", "R", "Missing"});
  result = rg::QueryExecutor(graph).Execute(scan);
  EXPECT_EQ(result.rows.size(), 5U);
  EXPECT_EQ(graph.type_scans, 1U);
  EXPECT_EQ(graph.candidates, 3U);
  EXPECT_EQ(graph.scans, 0U);
}

TEST(OpenCypherOptimizerTest, RangeSeeksOnlyReturnBoundedCandidates) {
  ObservedGraph graph;
  auto endpoint = graph.graph.CreateNode({});
  for (int i = 0; i < 256; ++i) {
    auto node = graph.graph.CreateNode({"N"}, {{"score", rg::Value(i)}});
    graph.graph.CreateRelationship(endpoint, node, "R",
                                   {{"score", rg::Value(i)}});
  }
  graph.graph.AddNodeIndex({"N"}, "score");
  graph.graph.AddRelationshipIndex({"R"}, "score");
  rg::QueryOptions options{
      .planner_catalog = &graph.graph,
      .parameters = {{"lower", rg::Value(100.0)}, {"upper", rg::Value(103)}}};
  for (const auto &query :
       {"MATCH (n:N) WHERE $lower <= n.score AND n.score > 99 AND "
        "$upper > n.score AND n.score < 200 RETURN n.score",
        "MATCH ()-[r:R]->() WHERE r.score >= $lower AND r.score < $upper "
        "RETURN r.score"}) {
    SCOPED_TRACE(query);
    graph.candidates = 0;
    graph.range_seeks = 0;
    const auto result = rg::ExecuteReadQuery(graph, query, options);
    EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{
                                {"100"}, {"101"}, {"102"}}));
    EXPECT_EQ(graph.range_seeks, 1U);
    EXPECT_EQ(graph.candidates, 3U);
  }
  EXPECT_EQ(graph.scans, 0U);
  EXPECT_EQ(graph.label_scans, 0U);
  EXPECT_EQ(graph.type_scans, 0U);
}

TEST(OpenCypherOptimizerTest, PrefixSeeksUseStringIndexIntervals) {
  ObservedGraph graph;
  auto endpoint = graph.graph.CreateNode({});
  for (const auto *name : {"", "a", "aa", "ab", "abc", "ac", "z"}) {
    auto node = graph.graph.CreateNode({"N"}, {{"name", rg::Value(name)}});
    graph.graph.CreateRelationship(endpoint, node, "R",
                                   {{"name", rg::Value(name)}});
  }
  graph.graph.CreateNode({"N"}, {{"name", rg::Value(42)}});
  graph.graph.AddNodeIndex({"N"}, "name");
  graph.graph.AddRelationshipIndex({"R"}, "name");
  rg::QueryOptions options{.planner_catalog = &graph.graph};
  for (const auto &query :
       {"MATCH (n:N) WHERE n.name STARTS WITH $prefix RETURN n.name",
        "MATCH ()-[r:R]->() WHERE r.name STARTS WITH $prefix RETURN r.name"}) {
    for (const auto &prefix :
         {rg::Value("ab"), rg::Value(""), rg::Value("missing"),
          rg::Value::Null(), rg::Value(42)}) {
      SCOPED_TRACE(query);
      SCOPED_TRACE(prefix.ToString());
      options.parameters = {{"prefix", prefix}};
      graph.candidates = 0;
      graph.range_seeks = 0;
      const auto result = rg::ExecuteReadQuery(graph, query, options);
      const std::size_t expected = prefix.IsString()
                                       ? (prefix.AsString() == "ab"   ? 2U
                                          : prefix.AsString().empty() ? 7U
                                                                      : 0U)
                                       : 0U;
      EXPECT_EQ(result.rows.size(), expected);
      EXPECT_EQ(graph.range_seeks, 1U);
      EXPECT_EQ(graph.candidates, expected);
    }
  }
  EXPECT_EQ(graph.scans, 0U);
}

TEST(OpenCypherOptimizerTest,
     KeepsRowDependentAndVolatileRangeBoundsAsFilters) {
  for (const auto &query : {"MATCH (n:N) WHERE n.x > n.y RETURN n",
                            "MATCH (n:N) WHERE n.x > rand() RETURN n",
                            "MATCH (n:N) WHERE n.x STARTS WITH n.y RETURN n"}) {
    SCOPED_TRACE(query);
    Query plan(query);
    EXPECT_EQ(Find(*plan.plan, Type::kNodeIndexRangeSeek), nullptr);
    EXPECT_NE(Find(*plan.plan, Type::kFilter), nullptr);
  }
  rg::InMemoryGraph graph;
  graph.CreateNode({"N"}, {{"x", rg::Value(3)}, {"y", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"x", rg::Value(1)}, {"y", rg::Value(2)}});
  graph.AddNodeIndex({"N"}, "x");
  rg::QueryOptions options{.planner_catalog = &graph};
  const auto result = rg::ExecuteReadQuery(
      graph, "MATCH (n:N) WHERE n.x >= 0 AND n.x > n.y RETURN n.x", options);
  EXPECT_EQ(Rows(result), (std::multiset<std::vector<std::string>>{{"3"}}));
}
