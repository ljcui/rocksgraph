#include "ir/logical_plan.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <unordered_set>

#include "common/exception.h"

namespace {

bool Contains(const std::unordered_set<std::string> &set,
              const std::string &value) {
  return set.find(value) != set.end();
}

}  // namespace

TEST(LogicalPlanNodeTypeTest, SupportsToStringAndOstream) {
  EXPECT_EQ(ir::ToString(ir::LogicalPlanNodeType::kSelection), "Selection");

  std::ostringstream out;
  out << ir::LogicalPlanNodeType::kCartesianProduct;
  EXPECT_EQ(out.str(), "CartesianProduct");
}

TEST(LogicalPlanLeafTest, AllNodesScanAndNodeByLabelScanExposeSymbols) {
  ir::AllNodesScan all_nodes("n", {"arg"});
  auto all_symbols = all_nodes.AvailableSymbols();
  EXPECT_TRUE(Contains(all_symbols, "arg"));
  EXPECT_TRUE(Contains(all_symbols, "n"));

  ir::NodeByLabelScan by_label("m", "Person", {"arg"});
  auto label_symbols = by_label.AvailableSymbols();
  EXPECT_TRUE(Contains(label_symbols, "arg"));
  EXPECT_TRUE(Contains(label_symbols, "m"));
}

TEST(LogicalPlanUnaryTest, ExpandAndProjectPropagateSymbols) {
  ast::BooleanLiteral predicate;
  predicate.value = true;
  ast::IntegerLiteral one;
  one.value = 1;

  auto plan = std::make_unique<ir::Project>(
      std::make_unique<ir::Selection>(
          std::make_unique<ir::Expand>(
              std::make_unique<ir::AllNodesScan>(
                  "n", std::unordered_set<std::string>{"arg"}),
              "n", "r", "m", ir::Expand::Direction::kOutgoing),
          std::vector<const ast::Expression *>{&predicate}),
      std::vector<ir::ProjectItem>{{&one, "m"}});

  auto symbols = plan->AvailableSymbols();
  EXPECT_EQ(symbols.size(), 1U);
  EXPECT_TRUE(Contains(symbols, "m"));
}

TEST(LogicalPlanBinaryTest, CartesianProductUnionsChildSymbols) {
  auto plan = std::make_unique<ir::CartesianProduct>(
      std::make_unique<ir::AllNodesScan>("a"),
      std::make_unique<ir::NodeByLabelScan>(
          "b", "Person", std::unordered_set<std::string>{"arg"}));
  auto symbols = plan->AvailableSymbols();
  EXPECT_TRUE(Contains(symbols, "a"));
  EXPECT_TRUE(Contains(symbols, "b"));
  EXPECT_TRUE(Contains(symbols, "arg"));
}

TEST(LogicalPlanBinaryTest, NodeHashJoinUnionsChildSymbols) {
  auto plan = std::make_unique<ir::NodeHashJoin>(
      std::make_unique<ir::AllNodesScan>("a"),
      std::make_unique<ir::AllNodesScan>("b"),
      std::unordered_set<std::string>{"a"});
  auto symbols = plan->AvailableSymbols();
  EXPECT_TRUE(Contains(symbols, "a"));
  EXPECT_TRUE(Contains(symbols, "b"));
}

TEST(LogicalPlanTreeTest, FlattenAndLeftmostLeafFollowPreOrder) {
  ast::BooleanLiteral predicate;
  predicate.value = true;
  ast::IntegerLiteral one;
  one.value = 1;

  auto plan = std::make_unique<ir::ProduceResult>(
      std::make_unique<ir::Limit>(
          std::make_unique<ir::Project>(
              std::make_unique<ir::Selection>(
                  std::make_unique<ir::AllNodesScan>("n"),  //
                  std::vector<const ast::Expression *>{&predicate}),
              std::vector<ir::ProjectItem>{{&one, "n"}}),
          &one),
      std::vector<std::string>{"n"});

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  ASSERT_EQ(flattened.size(), 5U);
  EXPECT_EQ(flattened[0]->node_type, ir::LogicalPlanNodeType::kProduceResult);
  EXPECT_EQ(flattened[1]->node_type, ir::LogicalPlanNodeType::kLimit);
  EXPECT_EQ(flattened[2]->node_type, ir::LogicalPlanNodeType::kProject);
  EXPECT_EQ(flattened[3]->node_type, ir::LogicalPlanNodeType::kSelection);
  EXPECT_EQ(flattened[4]->node_type, ir::LogicalPlanNodeType::kAllNodesScan);

  const ir::LogicalPlan &leaf = ir::LeftmostLeaf(*plan);
  EXPECT_EQ(leaf.node_type, ir::LogicalPlanNodeType::kAllNodesScan);
}

TEST(LogicalPlanValidationTest, RejectsInvalidInputs) {
  EXPECT_THROW((void)ir::AllNodesScan("", {}), common::InvalidArgumentError);
  EXPECT_THROW((void)ir::Selection(std::make_unique<ir::Argument>(),
                                   std::vector<const ast::Expression *>{}),
               common::InvalidArgumentError);
  EXPECT_THROW((void)ir::Project(std::make_unique<ir::Argument>(),
                                 std::vector<ir::ProjectItem>{{nullptr, "x"}}),
               common::InvalidArgumentError);
  EXPECT_THROW((void)ir::NodeHashJoin(std::make_unique<ir::AllNodesScan>("a"),
                                      std::make_unique<ir::AllNodesScan>("b"),
                                      std::unordered_set<std::string>{}),
               common::InvalidArgumentError);
}
