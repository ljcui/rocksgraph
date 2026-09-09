#include "value/value.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

void ExpectEquivalentKeys(const rg::Value &left, const rg::Value &right) {
  EXPECT_TRUE(rg::ValueEqual{}(left, right));
  EXPECT_TRUE(rg::ValueEqual{}(right, left));
  EXPECT_EQ(rg::ValueHash{}(left), rg::ValueHash{}(right));
}

}  // namespace

static_assert(std::is_const_v<typename rg::Value::NodePtr::element_type>);
static_assert(
    std::is_const_v<typename rg::Value::RelationshipPtr::element_type>);
static_assert(std::is_const_v<typename rg::Value::PathPtr::element_type>);
static_assert(std::is_const_v<std::remove_reference_t<
                  decltype(std::declval<rg::Value &>().AsNode())>>);
static_assert(std::is_const_v<std::remove_reference_t<
                  decltype(std::declval<rg::Value &>().AsRelationship())>>);
static_assert(std::is_const_v<std::remove_reference_t<
                  decltype(std::declval<rg::Value &>().AsPath())>>);

TEST(ValueTest, ScalarTypes) {
  rg::Value null_value;
  EXPECT_TRUE(null_value.IsNull());
  EXPECT_EQ(null_value.Type(), rg::ValueType::kNull);

  rg::Value bool_value(true);
  EXPECT_TRUE(bool_value.IsBool());
  EXPECT_EQ(bool_value.AsBool(), true);

  rg::Value int_value(42);
  EXPECT_TRUE(int_value.IsInteger());
  EXPECT_EQ(int_value.AsInteger(), 42);

  rg::Value double_value(3.5);
  EXPECT_TRUE(double_value.IsDouble());
  EXPECT_DOUBLE_EQ(double_value.AsDouble(), 3.5);

  rg::Value string_value("rocks");
  EXPECT_TRUE(string_value.IsString());
  EXPECT_EQ(string_value.AsString(), "rocks");
}

TEST(ValueTest, ListAndMapTypes) {
  rg::Value::List list{rg::Value(1), rg::Value("two"), rg::Value(3.0)};
  rg::Value list_value(list);

  EXPECT_TRUE(list_value.IsList());
  ASSERT_EQ(list_value.AsList().size(), 3U);
  EXPECT_EQ(list_value.AsList()[0], rg::Value(1));
  EXPECT_EQ(list_value.AsList()[1], rg::Value("two"));

  rg::Value::Map map{{"a", rg::Value(1)}, {"b", list_value}};
  rg::Value map_value(map);

  EXPECT_TRUE(map_value.IsMap());
  ASSERT_EQ(map_value.AsMap().size(), 2U);
  EXPECT_EQ(map_value.AsMap().at("a"), rg::Value(1));
  EXPECT_EQ(map_value.AsMap().at("b"), list_value);
}

TEST(ValueTest, QueryEqualityAndHashingNormalizeNumericValues) {
  EXPECT_TRUE(rg::ValuesEqual(rg::Value(1), rg::Value(1.0)));
  EXPECT_TRUE(rg::ValuesEqual(rg::Value(1.5), rg::Value(1.5)));
  EXPECT_FALSE(rg::ValuesEqual(rg::Value(1.5), rg::Value(2.5)));
  ExpectEquivalentKeys(rg::Value(1), rg::Value(1.0));
  ExpectEquivalentKeys(rg::Value(0), rg::Value(-0.0));
  ExpectEquivalentKeys(
      rg::Value(std::numeric_limits<std::int64_t>::min()),
      rg::Value(static_cast<double>(std::numeric_limits<std::int64_t>::min())));

  const rg::Value integer_list(rg::Value::List{rg::Value(1)});
  const rg::Value double_list(rg::Value::List{rg::Value(1.0)});
  EXPECT_TRUE(rg::ValuesEqual(integer_list, double_list));
  ExpectEquivalentKeys(integer_list, double_list);

  EXPECT_FALSE(rg::ValueEqual{}(rg::Value(1.0000001), rg::Value(1.0000002)));
  EXPECT_FALSE(
      rg::ValuesEqual(rg::Value(std::numeric_limits<std::int64_t>::max()),
                      rg::Value(9.223372036854776e18)));
}

TEST(ValueTest, HashKeyEqualityCanonicalizesNaN) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const rg::Value first(nan);
  const rg::Value second(-nan);

  EXPECT_FALSE(rg::ValuesEqual(first, second));
  ExpectEquivalentKeys(first, second);

  std::unordered_set<rg::Value, rg::ValueHash, rg::ValueEqual> values;
  values.insert(first);
  values.insert(second);
  EXPECT_EQ(values.size(), 1U);
}

TEST(ValueTest, HashesListsAndMapsRecursively) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const rg::Value left(rg::Value::Map{
      {"items", rg::Value(rg::Value::List{rg::Value(1), rg::Value(nan)})},
      {"name", rg::Value("Ada")}});
  const rg::Value right(rg::Value::Map{
      {"items", rg::Value(rg::Value::List{rg::Value(1.0), rg::Value(-nan)})},
      {"name", rg::Value("Ada")}});
  const rg::Value different(rg::Value::Map{
      {"items", rg::Value(rg::Value::List{rg::Value(1.0), rg::Value(-nan)})},
      {"name", rg::Value("Grace")}});

  ExpectEquivalentKeys(left, right);
  EXPECT_FALSE(rg::ValueEqual{}(left, different));
}

TEST(ValueTest, QueryEqualityUsesGraphEntityIdentity) {
  auto first_node = std::make_shared<rg::Node>();
  first_node->id = 7;
  first_node->labels = {"Before"};
  auto second_node = std::make_shared<rg::Node>();
  second_node->id = 7;
  second_node->labels = {"After"};

  auto first_relationship = std::make_shared<rg::Relationship>();
  first_relationship->id = 9;
  first_relationship->type = "BEFORE";
  auto second_relationship = std::make_shared<rg::Relationship>();
  second_relationship->id = 9;
  second_relationship->type = "AFTER";

  EXPECT_TRUE(rg::ValuesEqual(rg::Value(first_node), rg::Value(second_node)));
  ExpectEquivalentKeys(rg::Value(first_node), rg::Value(second_node));
  EXPECT_TRUE(rg::ValuesEqual(rg::Value(first_relationship),
                              rg::Value(second_relationship)));
  ExpectEquivalentKeys(rg::Value(first_relationship),
                       rg::Value(second_relationship));

  auto left_path = std::make_shared<rg::Path>();
  left_path->nodes = {first_node};
  left_path->relationships = {first_relationship};
  auto right_path = std::make_shared<rg::Path>();
  right_path->nodes = {second_node};
  right_path->relationships = {second_relationship};
  ExpectEquivalentKeys(rg::Value(left_path), rg::Value(right_path));
}

TEST(ValueTest, HashesTemporalDurationAndPointValues) {
  ExpectEquivalentKeys(rg::Value(rg::Date{2026, 9, 5}),
                       rg::Value(rg::Date{2026, 9, 5}));
  ExpectEquivalentKeys(rg::Value(rg::LocalTime{12, 30, 45, 123, true}),
                       rg::Value(rg::LocalTime{12, 30, 45, 123, false}));
  ExpectEquivalentKeys(
      rg::Value(rg::DateTime{
          {{2026, 9, 5}, {12, 30, 45, 123, true}}, 28'800, "Asia/Shanghai"}),
      rg::Value(rg::DateTime{
          {{2026, 9, 5}, {12, 30, 45, 123, false}}, 28'800, "Asia/Shanghai"}));
  ExpectEquivalentKeys(rg::Value(rg::Duration{1, 2, 3, 4}),
                       rg::Value(rg::Duration{1, 2, 3, 4}));

  const double nan = std::numeric_limits<double>::quiet_NaN();
  ExpectEquivalentKeys(rg::Value(rg::Point{4326, {0.0, nan}}),
                       rg::Value(rg::Point{4326, {-0.0, -nan}}));
  EXPECT_FALSE(rg::ValueEqual{}(rg::Value(rg::Point{4326, {1.0, 2.0}}),
                                rg::Value(rg::Point{7203, {1.0, 2.0}})));
}

TEST(ValueTest, HashesCompositeKeysInColumnOrder) {
  std::unordered_set<rg::CompositeValueKey, rg::ValueHash, rg::ValueEqual> keys;
  keys.insert({.values = {rg::Value(1), rg::Value("x")}});
  keys.insert({.values = {rg::Value(1.0), rg::Value("x")}});
  keys.insert({.values = {rg::Value("x"), rg::Value(1)}});

  EXPECT_EQ(keys.size(), 2U);
}

TEST(ValueTest, GraphTypes) {
  auto node = std::make_shared<rg::Node>();
  node->id = 7;
  node->labels = {"Person", "Employee"};
  node->properties["name"] = rg::Value("Ada");
  node->properties["age"] = rg::Value(30);

  auto rel = std::make_shared<rg::Relationship>();
  rel->id = 99;
  rel->start_node_id = 7;
  rel->end_node_id = 8;
  rel->type = "KNOWS";
  rel->properties["since"] = rg::Value(2020);

  auto path = std::make_shared<rg::Path>();
  path->nodes.push_back(node);
  path->relationships.push_back(rel);

  rg::Value node_value(node);
  rg::Value rel_value(rel);
  rg::Value path_value(path);

  EXPECT_TRUE(node_value.IsNode());
  EXPECT_TRUE(rel_value.IsRelationship());
  EXPECT_TRUE(path_value.IsPath());

  EXPECT_EQ(node_value.AsNode().id, 7);
  EXPECT_EQ(rel_value.AsRelationship().type, "KNOWS");
  EXPECT_EQ(path_value.AsPath().nodes.size(), 1U);

  rg::Value node_value_copy(std::make_shared<rg::Node>(*node));
  EXPECT_EQ(node_value, node_value_copy);
}

TEST(ValueTest, TemporalAndSpatialTypes) {
  rg::Date date{2024, 1, 2};
  rg::LocalTime local_time{10, 20, 30, 400};
  rg::Time time{local_time, 3600};
  rg::LocalDateTime local_date_time{date, local_time};
  rg::DateTime date_time{local_date_time, -3600};
  rg::Duration duration{1, 2, 3, 4};
  rg::Point point{4326, {1.0, 2.0}};

  rg::Value date_value(date);
  rg::Value time_value(time);
  rg::Value duration_value(duration);
  rg::Value point_value(point);

  EXPECT_TRUE(date_value.IsDate());
  EXPECT_TRUE(time_value.IsTime());
  EXPECT_TRUE(duration_value.IsDuration());
  EXPECT_TRUE(point_value.IsPoint());

  EXPECT_EQ(date_value.AsDate(), date);
  EXPECT_EQ(time_value.AsTime(), time);
  EXPECT_EQ(duration_value.AsDuration(), duration);
  EXPECT_EQ(point_value.AsPoint(), point);

  rg::Value local_date_time_value(local_date_time);
  rg::Value date_time_value(date_time);
  EXPECT_TRUE(local_date_time_value.IsLocalDateTime());
  EXPECT_TRUE(date_time_value.IsDateTime());
}
