#include "value/value.h"

#include <gtest/gtest.h>

TEST(ValueTest, ScalarTypes) {
  rg::Value null_value;
  EXPECT_TRUE(null_value.is_null());
  EXPECT_EQ(null_value.type(), rg::ValueType::kNull);

  rg::Value bool_value(true);
  EXPECT_TRUE(bool_value.is_bool());
  EXPECT_EQ(bool_value.as_bool(), true);

  rg::Value int_value(42);
  EXPECT_TRUE(int_value.is_integer());
  EXPECT_EQ(int_value.as_integer(), 42);

  rg::Value double_value(3.5);
  EXPECT_TRUE(double_value.is_double());
  EXPECT_DOUBLE_EQ(double_value.as_double(), 3.5);

  rg::Value string_value("rocks");
  EXPECT_TRUE(string_value.is_string());
  EXPECT_EQ(string_value.as_string(), "rocks");
}

TEST(ValueTest, ListAndMapTypes) {
  rg::Value::List list{rg::Value(1), rg::Value("two"), rg::Value(3.0)};
  rg::Value list_value(list);

  EXPECT_TRUE(list_value.is_list());
  ASSERT_EQ(list_value.as_list().size(), 3U);
  EXPECT_EQ(list_value.as_list()[0], rg::Value(1));
  EXPECT_EQ(list_value.as_list()[1], rg::Value("two"));

  rg::Value::Map map{{"a", rg::Value(1)}, {"b", list_value}};
  rg::Value map_value(map);

  EXPECT_TRUE(map_value.is_map());
  ASSERT_EQ(map_value.as_map().size(), 2U);
  EXPECT_EQ(map_value.as_map().at("a"), rg::Value(1));
  EXPECT_EQ(map_value.as_map().at("b"), list_value);
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

  EXPECT_TRUE(node_value.is_node());
  EXPECT_TRUE(rel_value.is_relationship());
  EXPECT_TRUE(path_value.is_path());

  EXPECT_EQ(node_value.as_node().id, 7);
  EXPECT_EQ(rel_value.as_relationship().type, "KNOWS");
  EXPECT_EQ(path_value.as_path().nodes.size(), 1U);

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

  EXPECT_TRUE(date_value.is_date());
  EXPECT_TRUE(time_value.is_time());
  EXPECT_TRUE(duration_value.is_duration());
  EXPECT_TRUE(point_value.is_point());

  EXPECT_EQ(date_value.as_date(), date);
  EXPECT_EQ(time_value.as_time(), time);
  EXPECT_EQ(duration_value.as_duration(), duration);
  EXPECT_EQ(point_value.as_point(), point);

  rg::Value local_date_time_value(local_date_time);
  rg::Value date_time_value(date_time);
  EXPECT_TRUE(local_date_time_value.is_local_date_time());
  EXPECT_TRUE(date_time_value.is_date_time());
}
