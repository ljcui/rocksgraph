#include <gtest/gtest.h>

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "common/exceptions.h"
#include "runtime/graphdb_access.h"
#include "runtime/graphdb_planner_catalog.h"
#include "runtime/query_executor.h"
#include "tests/runtime/graphdb_test_utils.h"

namespace {

std::vector<std::int64_t> VertexIds(
    std::unique_ptr<graphdb::VertexIterator> iterator) {
  std::vector<std::int64_t> ids;
  while (iterator->Valid()) {
    ids.push_back(iterator->GetVertex().GetNativeId());
    iterator->Next();
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::int64_t> EdgeIds(
    std::unique_ptr<graphdb::EdgeIterator> iterator) {
  std::vector<std::int64_t> ids;
  while (iterator->Valid()) {
    ids.push_back(iterator->GetEdge().GetNativeId());
    iterator->Next();
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::int64_t> VertexIdsByLabels(
    graphdb::Transaction& transaction,
    const std::unordered_set<std::string>& required_labels) {
  std::vector<std::int64_t> ids;
  auto iterator = transaction.NewVertexIterator();
  while (iterator->Valid()) {
    const auto labels = iterator->GetVertex().GetLabels();
    const bool matches =
        std::all_of(required_labels.begin(), required_labels.end(),
                    [&](const auto& label) { return labels.contains(label); });
    if (matches) {
      ids.push_back(iterator->GetVertex().GetNativeId());
    }
    iterator->Next();
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::int64_t> ResultIds(const rg::QueryResult& result) {
  std::vector<std::int64_t> ids;
  for (const auto& row : result.rows) {
    ids.push_back(row.front().AsInteger());
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

template <typename Getter>
void WaitForIndexBuild(Getter getter) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto index = getter();
    if (index != nullptr && index->IsReady()) {
      return;
    }
    if (index != nullptr && index->state() == meta::IndexBuildState::FAILED) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "GraphDB property index did not finish building";
}

}  // namespace

TEST(IndexAccessTest, TransactionExposesOneReadWriteView) {
  rg::test::GraphDBTestDatabase graph;
  auto transaction = graph.BeginTransaction();

  auto node =
      transaction->CreateVertex({"Person"}, {{"name", rg::Value("Ada")}});
  EXPECT_EQ(node.GetProperty("name"), rg::Value("Ada"));
  EXPECT_EQ(rg::GraphDBVertexById(*transaction, node.GetNativeId())
                .GetProperty("name"),
            rg::Value("Ada"));

  transaction->Rollback();
  auto verify = graph.BeginTransaction();
  EXPECT_FALSE(verify->NewVertexIterator()->Valid());
  verify->Rollback();
}

TEST(IndexAccessTest, TransactionRollsBackOnDestruction) {
  rg::test::GraphDBTestDatabase graph;
  {
    auto transaction = graph.BeginTransaction();
    transaction->CreateVertex({"Temporary"}, {});
  }

  auto verify = graph.BeginTransaction();
  EXPECT_FALSE(verify->NewVertexIterator()->Valid());
  verify->Rollback();

  auto next_transaction = graph.BeginTransaction();
  EXPECT_NO_THROW(next_transaction->Commit());
}

TEST(IndexAccessTest, MaintainsLabelsAndTypesAcrossMutationsAndRollback) {
  rg::test::GraphDBTestDatabase graph;
  const auto a = graph.CreateNode({"A", "A", "B"});
  const auto b = graph.CreateNode({"B"});
  const auto r = graph.CreateRelationship(a, b, "R");

  auto transaction = graph.BeginTransaction();
  EXPECT_EQ(VertexIdsByLabels(*transaction, {"A", "B"}),
            (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(VertexIdsByLabels(*transaction, {}),
            (std::vector<std::int64_t>{a->id, b->id}));
  EXPECT_EQ(VertexIds(transaction->NewVertexIterator("Missing")),
            (std::vector<std::int64_t>{}));
  EXPECT_EQ(EdgeIds(transaction->NewEdgeIterator({"R", "R", "Missing"})),
            (std::vector<std::int64_t>{r->id}));

  auto vertex_a = rg::GraphDBVertexById(*transaction, a->id);
  auto vertex_b = rg::GraphDBVertexById(*transaction, b->id);
  vertex_a.DeleteLabels({"A"});
  vertex_b.AddLabels({"A", "A"});
  vertex_b.AddLabels({"A"});
  EXPECT_EQ(VertexIdsByLabels(*transaction, {"A", "B"}),
            (std::vector<std::int64_t>{b->id}));

  auto relationship = rg::GraphDBEdgeById(*transaction, {r->id, r->type_id});
  relationship.Delete();
  vertex_a.Delete();
  auto vertex_c = transaction->CreateVertex({"A"}, {});
  auto relationship_s = transaction->CreateEdge(vertex_b, vertex_c, "S", {});
  EXPECT_EQ(EdgeIds(transaction->NewEdgeIterator({"R"})),
            (std::vector<std::int64_t>{}));
  EXPECT_EQ(EdgeIds(transaction->NewEdgeIterator({"R", "S"})),
            (std::vector<std::int64_t>{relationship_s.GetNativeId()}));
  transaction->Rollback();

  auto verify = graph.BeginTransaction();
  EXPECT_EQ(VertexIdsByLabels(*verify, {"A"}),
            (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(VertexIdsByLabels(*verify, {"B"}),
            (std::vector<std::int64_t>{a->id, b->id}));
  EXPECT_EQ(EdgeIds(verify->NewEdgeIterator()),
            (std::vector<std::int64_t>{r->id}));
  EXPECT_EQ(EdgeIds(verify->NewEdgeIterator({"S"})),
            (std::vector<std::int64_t>{}));
  verify->Rollback();
}

TEST(IndexAccessTest, MaintainsRangeIndexesAcrossWritesAndRollback) {
  rg::test::GraphDBTestDatabase graph;
  const auto a = graph.CreateNode({"N"}, {{"value", rg::Value(10)}});
  const auto b = graph.CreateNode({"N"}, {{"value", rg::Value(20)}});
  const auto r =
      graph.CreateRelationship(a, b, "R", {{"value", rg::Value(10)}});
  const std::string node_index = graph.AddNodeIndex({"N"}, "value");
  const std::string edge_index = graph.AddRelationshipIndex({"R"}, "value");

  auto transaction = graph.BeginTransaction();
  auto nodes = [&] {
    return VertexIds(transaction->QueryVertexByPropertyRange(
        node_index, rg::Value(10), rg::Value(15), true, true));
  };
  auto relationships = [&] {
    return EdgeIds(transaction->QueryEdgeByPropertyRange(
        edge_index, rg::Value(10), rg::Value(15), true, true));
  };
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(relationships(), (std::vector<std::int64_t>{r->id}));

  auto vertex_a = rg::GraphDBVertexById(*transaction, a->id);
  auto vertex_b = rg::GraphDBVertexById(*transaction, b->id);
  auto edge_r = rg::GraphDBEdgeById(*transaction, {r->id, r->type_id});
  vertex_a.RemoveAllProperty();
  vertex_a.SetProperties({{"value", rg::Value(30)}});
  vertex_b.SetProperties({{"value", rg::Value(12)}});
  edge_r.SetProperties({{"value", rg::Value(12)}});
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{b->id}));
  EXPECT_EQ(relationships(), (std::vector<std::int64_t>{r->id}));
  vertex_b.DeleteLabels({"N"});
  EXPECT_TRUE(nodes().empty());
  vertex_b.AddLabels({"N"});
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{b->id}));
  vertex_b.RemoveProperty("value");
  edge_r.RemoveProperty("value");
  EXPECT_TRUE(nodes().empty());
  EXPECT_TRUE(relationships().empty());
  auto vertex_c = transaction->CreateVertex({"N"}, {{"value", rg::Value(11)}});
  auto edge_s = transaction->CreateEdge(vertex_a, vertex_c, "R",
                                        {{"value", rg::Value(11)}});
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{vertex_c.GetNativeId()}));
  EXPECT_EQ(relationships(), (std::vector<std::int64_t>{edge_s.GetNativeId()}));
  edge_s.Delete();
  vertex_c.Delete();
  EXPECT_TRUE(nodes().empty());
  EXPECT_TRUE(relationships().empty());
  transaction->Rollback();

  auto verify = graph.BeginTransaction();
  EXPECT_EQ(VertexIds(verify->QueryVertexByPropertyRange(
                node_index, rg::Value(10), rg::Value(15), true, true)),
            (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(EdgeIds(verify->QueryEdgeByPropertyRange(
                edge_index, rg::Value(10), rg::Value(15), true, true)),
            (std::vector<std::int64_t>{r->id}));
  verify->Rollback();
}

TEST(IndexAccessTest, RangeResultsMatchFiltersAcrossValueTypes) {
  using rg::Value;
  const std::vector<std::vector<Value>> value_groups = {
      {Value::Null()},
      {Value(false), Value(true)},
      {Value(std::numeric_limits<std::int64_t>::min()), Value(-1), Value(0),
       Value(10), Value(std::numeric_limits<std::int64_t>::max())},
      {Value(-10.5), Value(-1.0), Value(0.0), Value(10.5), Value(20.0)},
      {Value(""), Value("a"), Value("ab"), Value("b")},
      {Value(rg::Date{2023, 1, 1}), Value(rg::Date{2024, 1, 1}),
       Value(rg::Date{2025, 1, 1})}};

  for (const auto& values : value_groups) {
    rg::test::GraphDBTestDatabase graph;
    const auto endpoint = graph.CreateNode({});
    for (const auto& value : values) {
      const auto node = graph.CreateNode({"N"}, {{"value", value}});
      graph.CreateRelationship(endpoint, node, "R", {{"value", value}});
    }
    graph.AddNodeIndex({"N"}, "value");
    graph.AddRelationshipIndex({"R"}, "value");

    ir::EmptyPlannerCatalog empty_catalog;
    rg::GraphDBPlannerCatalog graphdb_catalog(graph.Graph());
    for (const auto& bound : values) {
      for (const auto* op : {"<", "<=", ">", ">="}) {
        for (const auto* pattern : {"(e:N)", "()-[e:R]->()"}) {
          const auto query = std::string("MATCH ") + pattern +
                             " WHERE e.value " + op + " $bound RETURN id(e)";
          SCOPED_TRACE(query);
          SCOPED_TRACE(bound.ToString());
          rg::QueryOptions scan_options;
          scan_options.parameters = {{"bound", bound}};
          scan_options.planner_catalog = &empty_catalog;
          rg::QueryOptions seek_options;
          seek_options.parameters = {{"bound", bound}};
          seek_options.planner_catalog = &graphdb_catalog;
          EXPECT_EQ(ResultIds(rg::test::ExecuteQueryAndCommit(graph, query,
                                                              seek_options)),
                    ResultIds(rg::test::ExecuteQueryAndCommit(graph, query,
                                                              scan_options)));
        }
      }
    }
  }
}

TEST(IndexAccessTest, IntersectsBoundsAndHandlesEmptyRanges) {
  rg::test::GraphDBTestDatabase graph;
  for (int i = 0; i < 30; ++i) {
    graph.CreateNode({"N"}, {{"value", rg::Value(i)}});
  }
  const std::string index_name = graph.AddNodeIndex({"N"}, "value");

  auto query_range = [&](const std::optional<rg::Value>& lower,
                         const std::optional<rg::Value>& upper,
                         bool left_closed, bool right_closed) {
    auto transaction = graph.BeginTransaction();
    auto ids = VertexIds(transaction->QueryVertexByPropertyRange(
        index_name, lower, upper, left_closed, right_closed));
    transaction->Rollback();
    return ids;
  };
  EXPECT_TRUE(query_range(rg::Value(20), rg::Value(10), true, true).empty());
  EXPECT_TRUE(query_range(rg::Value(10), rg::Value(10), false, true).empty());
  EXPECT_EQ(query_range(rg::Value(10), rg::Value(12), false, true),
            (std::vector<std::int64_t>{12, 13}));
  EXPECT_EQ(query_range(std::nullopt, std::nullopt, true, true).size(), 30U);
}

TEST(IndexAccessTest, DeletesNodeAndRelationships) {
  rg::test::GraphDBTestDatabase graph;
  const auto a = graph.CreateNode({"N"});
  const auto b = graph.CreateNode({"N"});
  const auto relationship = graph.CreateRelationship(a, b, "R");

  auto transaction = graph.BeginTransaction();
  auto vertex_a = rg::GraphDBVertexById(*transaction, a->id);
  EXPECT_EQ(vertex_a.Delete(), 1);
  EXPECT_THROW(rg::GraphDBVertexById(*transaction, a->id), LgraphException);
  EXPECT_TRUE(EdgeIds(transaction->NewEdgeIterator()).empty());
  transaction->Commit();

  auto verify = graph.BeginTransaction();
  EXPECT_EQ(VertexIds(verify->NewVertexIterator()),
            (std::vector<std::int64_t>{b->id}));
  EXPECT_THROW(
      verify->GetEdgeById(relationship->type_id,
                          boost::endian::native_to_big(relationship->id)),
      LgraphException);
  verify->Rollback();
}

TEST(IndexAccessTest, EnforcesUniqueIndexesOnCreateAndUpdate) {
  rg::test::GraphDBTestDatabase graph;
  const auto first = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  const std::string index_name = graph.AddNodeIndex({"N"}, "key", true);

  {
    auto transaction = graph.BeginTransaction();
    EXPECT_THROW(transaction->CreateVertex({"N"}, {{"key", rg::Value(1)}}),
                 LgraphException);
    transaction->Rollback();
  }
  const auto second = graph.CreateNode({"N"}, {{"key", rg::Value(2)}});
  EXPECT_GT(second->id, first->id);

  {
    auto transaction = graph.BeginTransaction();
    auto second_vertex = rg::GraphDBVertexById(*transaction, second->id);
    EXPECT_THROW(second_vertex.SetProperties({{"key", rg::Value(1)}}),
                 LgraphException);
    transaction->Rollback();
  }
  auto verify = graph.BeginTransaction();
  EXPECT_EQ(rg::GraphDBVertexById(*verify, second->id).GetProperty("key"),
            rg::Value(2));
  EXPECT_EQ(
      VertexIds(verify->QueryVertexByPropertyIndex(index_name, rg::Value(1))),
      (std::vector<std::int64_t>{first->id}));
  verify->Rollback();

  const auto other = graph.CreateNode({"Other"}, {{"key", rg::Value(1)}});
  auto label_transaction = graph.BeginTransaction();
  auto other_vertex = rg::GraphDBVertexById(*label_transaction, other->id);
  EXPECT_THROW(other_vertex.AddLabels({"N"}), LgraphException);
  label_transaction->Rollback();
  auto label_verify = graph.BeginTransaction();
  EXPECT_EQ(rg::GraphDBVertexById(*label_verify, other->id).GetLabels(),
            (std::unordered_set<std::string>{"Other"}));
  label_verify->Rollback();
}

TEST(IndexAccessTest, EnforcesUniqueRelationshipIndexes) {
  rg::test::GraphDBTestDatabase graph;
  const auto a = graph.CreateNode({});
  const auto b = graph.CreateNode({});
  const auto first =
      graph.CreateRelationship(a, b, "R", {{"key", rg::Value(1)}});
  const std::string index_name = graph.AddRelationshipIndex({"R"}, "key", true);

  {
    auto transaction = graph.BeginTransaction();
    auto start = rg::GraphDBVertexById(*transaction, a->id);
    auto end = rg::GraphDBVertexById(*transaction, b->id);
    EXPECT_THROW(
        transaction->CreateEdge(start, end, "R", {{"key", rg::Value(1)}}),
        LgraphException);
    transaction->Rollback();
  }
  const auto second =
      graph.CreateRelationship(a, b, "R", {{"key", rg::Value(2)}});
  {
    auto transaction = graph.BeginTransaction();
    auto second_edge =
        rg::GraphDBEdgeById(*transaction, {second->id, second->type_id});
    EXPECT_THROW(second_edge.SetProperties({{"key", rg::Value(1)}}),
                 LgraphException);
    transaction->Rollback();
  }
  auto verify = graph.BeginTransaction();
  EXPECT_EQ(EdgeIds(verify->QueryEdgeByPropertyIndex(index_name, rg::Value(1))),
            (std::vector<std::int64_t>{first->id}));
  verify->Rollback();
}

TEST(IndexAccessTest, RejectsUniqueIndexesOverExistingDuplicates) {
  rg::test::GraphDBTestDatabase graph;
  const auto first = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  const auto second = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  graph.CreateRelationship(first, second, "R", {{"key", rg::Value(1)}});
  graph.CreateRelationship(second, first, "R", {{"key", rg::Value(1)}});

  const std::string node_index = "duplicate_node_unique_index";
  graph.Graph().AddVertexPropertyIndex(node_index, true, "N", {"key"});
  WaitForIndexBuild([&] {
    return graph.Graph().meta_info().GetVertexPropertyIndex(node_index);
  });
  const auto failed_node_index =
      graph.Graph().meta_info().GetVertexPropertyIndex(node_index);
  ASSERT_NE(failed_node_index, nullptr);
  EXPECT_EQ(failed_node_index->state(), meta::IndexBuildState::FAILED);

  const std::string edge_index = "duplicate_edge_unique_index";
  graph.Graph().AddEdgePropertyIndex(edge_index, true, "R", {"key"});
  WaitForIndexBuild([&] {
    return graph.Graph().meta_info().GetEdgePropertyIndex(edge_index);
  });
  const auto failed_edge_index =
      graph.Graph().meta_info().GetEdgePropertyIndex(edge_index);
  ASSERT_NE(failed_edge_index, nullptr);
  EXPECT_EQ(failed_edge_index->state(), meta::IndexBuildState::FAILED);
}

TEST(IndexAccessTest, UniqueIndexCreationIsAtomicWhenReplacingAnIndex) {
  rg::test::GraphDBTestDatabase graph;
  const auto first = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  const auto second = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  const std::string index_name = graph.AddNodeIndex({"N"}, "key");

  EXPECT_THROW(graph.Graph().AddVertexPropertyIndex("replacement_unique_index",
                                                    true, "N", {"key"}),
               LgraphException);
  const auto index =
      graph.Graph().meta_info().GetVertexPropertyIndex(index_name);
  ASSERT_NE(index, nullptr);
  EXPECT_FALSE(index->is_unique());
  auto transaction = graph.BeginTransaction();
  EXPECT_EQ(VertexIds(transaction->QueryVertexByPropertyIndex(index_name,
                                                              rg::Value(1))),
            (std::vector<std::int64_t>{first->id, second->id}));
  transaction->Rollback();
}

TEST(IndexAccessTest, IndexSchemasRemainDistinct) {
  rg::test::GraphDBTestDatabase graph;
  const auto labeled = graph.CreateNode({"L"}, {{"p", rg::Value(1)}});
  const auto other = graph.CreateNode({"M"}, {{"p\nL", rg::Value(2)}});
  const std::string first_index = graph.AddNodeIndex({"L"}, "p");
  const std::string second_index = graph.AddNodeIndex({"M"}, "p\nL");

  auto transaction = graph.BeginTransaction();
  EXPECT_EQ(VertexIds(transaction->QueryVertexByPropertyIndex(first_index,
                                                              rg::Value(1))),
            (std::vector<std::int64_t>{labeled->id}));
  EXPECT_EQ(VertexIds(transaction->QueryVertexByPropertyIndex(second_index,
                                                              rg::Value(2))),
            (std::vector<std::int64_t>{other->id}));
  transaction->Rollback();
}
