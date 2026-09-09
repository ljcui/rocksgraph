#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "common/exception.h"
#include "runtime/query_executor.h"
#include "storage/in_memory_graph.h"

namespace {

std::vector<std::int64_t> Ids(std::unique_ptr<rg::EntityIdCursor> cursor) {
  std::vector<std::int64_t> ids;
  while (cursor->Next()) {
    ids.push_back(cursor->Id());
  }
  cursor->Close();
  EXPECT_FALSE(cursor->Next());
  EXPECT_THROW((void)cursor->Id(), common::InvalidArgumentError);
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::int64_t> ResultIds(const rg::QueryResult &result) {
  std::vector<std::int64_t> ids;
  for (const auto &row : result.rows) {
    ids.push_back(row.front().AsInteger());
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

rg::IndexRange ClosedRange(int lower, int upper) {
  return {.lower_bounds = {{rg::Value(lower), true}},
          .upper_bounds = {{rg::Value(upper), true}}};
}

}  // namespace

TEST(IndexAccessTest, TransactionExposesOneReadWriteView) {
  rg::InMemoryGraph graph;
  auto transaction = graph.BeginTransaction();

  const rg::GraphReader &reader = transaction->Reader();
  rg::Storage *writer = transaction->Writer();
  ASSERT_NE(writer, nullptr);
  auto node = writer->CreateNode({"Person"}, {{"name", rg::Value("Ada")}});

  ASSERT_NE(reader.NodeById(node->id), nullptr);
  EXPECT_EQ(reader.NodeProperty(node->id, "name"), rg::Value("Ada"));

  transaction->Rollback();
  EXPECT_TRUE(graph.Nodes().empty());
}

TEST(IndexAccessTest, TransactionRollsBackOnDestructionAndRejectsOverlap) {
  rg::InMemoryGraph graph;
  {
    auto transaction = graph.BeginTransaction();
    ASSERT_NE(transaction->Writer(), nullptr);
    transaction->Writer()->CreateNode({"Temporary"});
    EXPECT_THROW((void)graph.BeginTransaction(), common::InvalidArgumentError);
  }

  EXPECT_TRUE(graph.Nodes().empty());
  std::unique_ptr<rg::StorageTransaction> next_transaction;
  EXPECT_NO_THROW(next_transaction = graph.BeginTransaction());
  ASSERT_NE(next_transaction, nullptr);
  next_transaction->Commit();
}

TEST(IndexAccessTest, MaintainsLabelsAndTypesAcrossMutationsAndRollback) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({"A", "A", "B"});
  auto b = graph.CreateNode({"B"});
  auto r = graph.CreateRelationship(a, b, "R");
  EXPECT_EQ(Ids(graph.ScanNodeIdsByLabels({"A", "B", "A"})),
            (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(Ids(graph.ScanNodeIdsByLabels({})),
            (std::vector<std::int64_t>{a->id, b->id}));
  EXPECT_TRUE(Ids(graph.ScanNodeIdsByLabels({"Missing"})).empty());
  EXPECT_EQ(Ids(graph.ScanRelationshipIdsByTypes({"R", "R", "Missing"})),
            (std::vector<std::int64_t>{r->id}));

  auto transaction = graph.BeginTransaction();
  graph.RemoveLabels(a->id, {"A"});
  graph.SetLabels(b->id, {"A", "A"});
  graph.SetLabels(b->id, {"A"});
  EXPECT_EQ(Ids(graph.ScanNodeIdsByLabels({"A", "B"})),
            (std::vector<std::int64_t>{b->id}));
  graph.DeleteRelationship(r->id);
  graph.DeleteNode(a->id);
  auto c = graph.CreateNode({"A"});
  auto s = graph.CreateRelationship(b, c, "S");
  EXPECT_TRUE(Ids(graph.ScanRelationshipIdsByTypes({"R"})).empty());
  EXPECT_EQ(Ids(graph.ScanRelationshipIdsByTypes({"R", "S"})),
            (std::vector<std::int64_t>{s->id}));
  transaction->Rollback();

  EXPECT_EQ(Ids(graph.ScanNodeIdsByLabels({"A"})),
            (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(Ids(graph.ScanNodeIdsByLabels({"B"})),
            (std::vector<std::int64_t>{a->id, b->id}));
  EXPECT_EQ(Ids(graph.ScanRelationshipIdsByTypes({})),
            (std::vector<std::int64_t>{r->id}));
  EXPECT_TRUE(Ids(graph.ScanRelationshipIdsByTypes({"S"})).empty());
}

TEST(IndexAccessTest, MaintainsRangeIndexesAcrossWritesAndRollback) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({"N"}, {{"value", rg::Value(10)}});
  auto b = graph.CreateNode({"N"}, {{"value", rg::Value(20)}});
  auto r = graph.CreateRelationship(a, b, "R", {{"value", rg::Value(10)}});
  graph.AddNodeIndex({"N"}, "value");
  graph.AddRelationshipIndex({"R"}, "value");
  const auto range = ClosedRange(10, 15);
  auto nodes = [&] {
    return Ids(graph.FindNodeIdsByIndexRange({"N"}, "value", range));
  };
  auto relationships = [&] {
    return Ids(graph.FindRelationshipIdsByIndexRange({"R"}, "value", range));
  };
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(relationships(), (std::vector<std::int64_t>{r->id}));

  auto transaction = graph.BeginTransaction();
  graph.SetNodeProperties(a->id, {{"value", rg::Value(30)}}, false);
  graph.SetNodeProperty(b->id, "value", rg::Value(12.0));
  graph.SetRelationshipProperties(r->id, {{"value", rg::Value(12)}}, false);
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{b->id}));
  EXPECT_EQ(relationships(), (std::vector<std::int64_t>{r->id}));
  graph.RemoveLabels(b->id, {"N"});
  EXPECT_TRUE(nodes().empty());
  graph.SetLabels(b->id, {"N"});
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{b->id}));
  graph.RemoveNodeProperty(b->id, "value");
  graph.SetRelationshipProperty(r->id, "value", rg::Value::Null());
  EXPECT_TRUE(nodes().empty());
  EXPECT_TRUE(relationships().empty());
  auto c = graph.CreateNode({"N"}, {{"value", rg::Value(11)}});
  auto s = graph.CreateRelationship(a, c, "R", {{"value", rg::Value(11)}});
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{c->id}));
  EXPECT_EQ(relationships(), (std::vector<std::int64_t>{s->id}));
  graph.DeleteRelationship(s->id);
  graph.DeleteNode(c->id);
  EXPECT_TRUE(nodes().empty());
  EXPECT_TRUE(relationships().empty());
  transaction->Rollback();
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(relationships(), (std::vector<std::int64_t>{r->id}));

  graph.AddNodeIndex({"N"}, "value");
  graph.AddRelationshipIndex({"R"}, "value");
  EXPECT_EQ(nodes(), (std::vector<std::int64_t>{a->id}));
  EXPECT_EQ(relationships(), (std::vector<std::int64_t>{r->id}));
}

TEST(IndexAccessTest, RangeResultsMatchFiltersAcrossValueTypes) {
  using rg::Value;
  const std::vector<Value> values = {
      Value::Null(),
      Value(false),
      Value(true),
      Value(-1),
      Value(0.0),
      Value(10),
      Value(10.0),
      Value(10.5),
      Value(20),
      Value(std::numeric_limits<std::int64_t>::min()),
      Value(std::numeric_limits<std::int64_t>::max()),
      Value(std::int64_t{9007199254740993}),
      Value(9007199254740992.0),
      Value(-std::numeric_limits<double>::infinity()),
      Value(std::numeric_limits<double>::infinity()),
      Value(std::numeric_limits<double>::quiet_NaN()),
      Value(""),
      Value("a"),
      Value("ab"),
      Value("b"),
      Value(Value::List{Value(1)}),
      Value(Value::List{Value(1.0)}),
      Value(Value::List{Value::Null()}),
      Value(Value::List{Value(2)}),
      Value(Value::Map{{"x", Value(1)}}),
      Value(Value::Map{{"x", Value(1.0)}}),
      Value(rg::Date{2024, 1, 1})};
  rg::InMemoryGraph graph;
  auto endpoint = graph.CreateNode({});
  graph.AddNodeIndex({"N"}, "value");
  graph.AddRelationshipIndex({"R"}, "value");
  for (const auto &value : values) {
    auto node = graph.CreateNode({"N"}, {{"value", value}});
    graph.CreateRelationship(endpoint, node, "R", {{"value", value}});
  }
  for (const auto &bound : values) {
    for (const auto *op : {"<", "<=", ">", ">="}) {
      for (const auto *pattern : {"(e:N)", "()-[e:R]->()"}) {
        const auto query = std::string("MATCH ") + pattern + " WHERE e.value " +
                           op + " $bound RETURN id(e)";
        SCOPED_TRACE(query);
        SCOPED_TRACE(bound.ToString());
        rg::QueryOptions scan_options{.parameters = {{"bound", bound}}};
        auto seek_options = scan_options;
        seek_options.planner_catalog = &graph;
        EXPECT_EQ(ResultIds(rg::ExecuteReadQuery(graph, query, seek_options)),
                  ResultIds(rg::ExecuteReadQuery(graph, query, scan_options)));
      }
    }
  }
}

TEST(IndexAccessTest, IntersectsBoundsAndHandlesEmptyRanges) {
  rg::InMemoryGraph graph;
  for (int i = 0; i < 30; ++i) {
    graph.CreateNode({"N"}, {{"value", rg::Value(i)}});
  }
  graph.AddNodeIndex({"N"}, "value");
  for (const auto &range :
       {ClosedRange(20, 10),
        rg::IndexRange{.lower_bounds = {{rg::Value(10), false}},
                       .upper_bounds = {{rg::Value(10), true}}},
        rg::IndexRange{.lower_bounds = {{rg::Value::Null(), true}}},
        rg::IndexRange{
            .lower_bounds = {{rg::Value(10), true}, {rg::Value("a"), true}}}}) {
    EXPECT_TRUE(
        Ids(graph.FindNodeIdsByIndexRange({"N"}, "value", range)).empty());
  }
  rg::IndexRange range{
      .lower_bounds = {{rg::Value(0), true}, {rg::Value(10), false}},
      .upper_bounds = {{rg::Value(20), true}, {rg::Value(12.0), true}}};
  EXPECT_EQ(Ids(graph.FindNodeIdsByIndexRange({"N"}, "value", range)),
            (std::vector<std::int64_t>{11, 12}));
  EXPECT_EQ(
      Ids(graph.FindNodeIdsByIndexRange({"N"}, "value", ClosedRange(10, 10))),
      (std::vector<std::int64_t>{10}));
  auto cursor = graph.FindNodeIdsByIndexRange({"N"}, "value", range);
  ASSERT_TRUE(cursor->Next());
  cursor->Close();
  EXPECT_FALSE(cursor->Next());
}

TEST(IndexAccessTest, RejectsDeletingNodeWithRelationships) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({"N"});
  auto b = graph.CreateNode({"N"});
  auto relationship = graph.CreateRelationship(a, b, "R");

  EXPECT_THROW(graph.DeleteNode(a->id), common::InvalidArgumentError);
  EXPECT_EQ(graph.RelationshipCount(), 1U);
  EXPECT_EQ(graph.NodeById(a->id)->id, a->id);

  graph.DeleteRelationship(relationship->id);
  EXPECT_NO_THROW(graph.DeleteNode(a->id));
  EXPECT_THROW((void)graph.NodeById(a->id), common::NotFoundError);
}

TEST(IndexAccessTest, EnforcesUniqueIndexesOnCreateAndUpdate) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  graph.AddNodeIndex({"N"}, "key", true);
  EXPECT_THROW(graph.CreateNode({"N"}, {{"key", rg::Value(1)}}),
               common::InvalidArgumentError);

  auto second = graph.CreateNode({"N"}, {{"key", rg::Value(2)}});
  EXPECT_EQ(second->id, first->id + 1);
  EXPECT_THROW(graph.SetNodeProperty(second->id, "key", rg::Value(1)),
               common::InvalidArgumentError);
  EXPECT_EQ(graph.NodeProperty(second->id, "key"), rg::Value(2));
  EXPECT_EQ(Ids(graph.FindNodeIdsByIndex({"N"}, "key", rg::Value(1))),
            (std::vector<std::int64_t>{first->id}));

  auto other = graph.CreateNode({"Other"}, {{"key", rg::Value(1)}});
  EXPECT_THROW(graph.SetLabels(other->id, {"N"}), common::InvalidArgumentError);
  EXPECT_EQ(other->labels, std::vector<std::string>{"Other"});
  EXPECT_EQ(Ids(graph.FindNodeIdsByIndex({"N"}, "key", rg::Value(1))),
            (std::vector<std::int64_t>{first->id}));
}

TEST(IndexAccessTest, EnforcesUniqueRelationshipIndexes) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({});
  auto b = graph.CreateNode({});
  auto first = graph.CreateRelationship(a, b, "R", {{"key", rg::Value(1)}});
  graph.AddRelationshipIndex({"R"}, "key", true);
  EXPECT_THROW(graph.CreateRelationship(a, b, "R", {{"key", rg::Value(1)}}),
               common::InvalidArgumentError);

  auto second = graph.CreateRelationship(a, b, "R", {{"key", rg::Value(2)}});
  EXPECT_EQ(second->id, first->id + 1);
  EXPECT_THROW(graph.SetRelationshipProperty(second->id, "key", rg::Value(1)),
               common::InvalidArgumentError);
  EXPECT_EQ(graph.RelationshipProperty(second->id, "key"), rg::Value(2));
  EXPECT_EQ(Ids(graph.FindRelationshipIdsByIndex({"R"}, "key", rg::Value(1))),
            (std::vector<std::int64_t>{first->id}));
}

TEST(IndexAccessTest, RejectsUniqueIndexesOverExistingDuplicates) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  auto b = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  graph.CreateRelationship(a, b, "R", {{"key", rg::Value(1)}});
  graph.CreateRelationship(b, a, "R", {{"key", rg::Value(1)}});

  EXPECT_THROW(graph.AddNodeIndex({"N"}, "key", true),
               common::InvalidArgumentError);
  EXPECT_THROW(graph.AddRelationshipIndex({"R"}, "key", true),
               common::InvalidArgumentError);
  EXPECT_FALSE(graph.FindNodeIndex({"N"}, "key").has_value());
  EXPECT_FALSE(graph.FindRelationshipIndex({"R"}, "key").has_value());
}

TEST(IndexAccessTest, UniqueIndexCreationIsAtomicWhenReplacingAnIndex) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  auto second = graph.CreateNode({"N"}, {{"key", rg::Value(1)}});
  graph.AddNodeIndex({"N"}, "key");

  EXPECT_THROW(graph.AddNodeIndex({"N"}, "key", true),
               common::InvalidArgumentError);
  ASSERT_TRUE(graph.FindNodeIndex({"N"}, "key").has_value());
  EXPECT_FALSE(graph.FindNodeIndex({"N"}, "key")->unique);
  EXPECT_EQ(Ids(graph.FindNodeIdsByIndex({"N"}, "key", rg::Value(1))),
            (std::vector<std::int64_t>{first->id, second->id}));
}

TEST(IndexAccessTest, IndexKeySeparatesEmbeddedDelimiters) {
  rg::InMemoryGraph graph;
  auto labeled = graph.CreateNode({"L"}, {{"p", rg::Value(1)}});
  auto unqualified = graph.CreateNode({}, {{"p\nL", rg::Value(2)}});
  graph.AddNodeIndex({"L"}, "p");
  graph.AddNodeIndex({}, "p\nL");

  EXPECT_EQ(Ids(graph.FindNodeIdsByIndex({"L", "L"}, "p", rg::Value(1))),
            (std::vector<std::int64_t>{labeled->id}));
  EXPECT_EQ(Ids(graph.FindNodeIdsByIndex({}, "p\nL", rg::Value(2))),
            (std::vector<std::int64_t>{unqualified->id}));
}
