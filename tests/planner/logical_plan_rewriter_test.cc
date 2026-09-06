#include "planner/logical_plan_rewriter.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_node.h"
#include "common/exception.h"
#include "ir/query_ir.h"
#include "planner/logical_plan_builder.h"

namespace {

class ReplaceAllNodeScanRule final : public ir::LogicalPlanRewriteRule {
 public:
  [[nodiscard]] bool Apply(ir::LogicalPlanPtr *plan) const override {
    if ((*plan)->Type() != ir::LogicalPlanNodeType::kAllNodeScan) {
      return false;
    }
    const auto &scan = static_cast<const ir::AllNodeScanPlan &>(**plan);
    *plan =
        std::make_unique<ir::NodeByLabelScanPlan>(scan.Variable(), "Person");
    return true;
  }
};

class ChangeSchemaRule final : public ir::LogicalPlanRewriteRule {
 public:
  [[nodiscard]] bool Apply(ir::LogicalPlanPtr *plan) const override {
    if ((*plan)->Type() != ir::LogicalPlanNodeType::kAllNodeScan) {
      return false;
    }
    *plan = std::make_unique<ir::AllNodeScanPlan>("other");
    return true;
  }
};

const ir::LogicalPlan *Find(const ir::LogicalPlan &plan,
                            ir::LogicalPlanNodeType type) {
  if (plan.Type() == type) {
    return &plan;
  }
  for (const auto &child : plan.Children()) {
    if (const auto *found = Find(*child, type); found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

TEST(LogicalPlanRewriterTest, AppliesCustomRulesBottomUp) {
  auto predicate = std::make_unique<ast::BooleanLiteral>();
  predicate->value = true;
  ir::LogicalPlanPtr plan = std::make_unique<ir::FilterPlan>(
      std::make_unique<ir::AllNodeScanPlan>("n"), predicate.get());
  ir::LogicalPlanRewritePipeline pipeline;
  pipeline.Add(std::make_unique<ReplaceAllNodeScanRule>());

  plan = pipeline.Run(std::move(plan));

  ASSERT_EQ(plan->Type(), ir::LogicalPlanNodeType::kFilter);
  ASSERT_EQ(plan->Child(0).Type(), ir::LogicalPlanNodeType::kNodeByLabelScan);
  EXPECT_EQ(plan->OutputColumns(), (std::vector<std::string>{"n"}));
}

TEST(LogicalPlanRewriterTest, RejectsRulesThatChangeThePlanSchema) {
  ir::LogicalPlanRewritePipeline pipeline;
  pipeline.Add(std::make_unique<ChangeSchemaRule>());

  EXPECT_THROW((void)pipeline.Run(std::make_unique<ir::AllNodeScanPlan>("n")),
               common::InternalError);
}

TEST(LogicalPlanRewriterTest, DefaultPipelinePrunesDistinctVarExpand) {
  std::unique_ptr<ast::Statement> statement = ast::ParseCypherAndRewrite(
      "MATCH (a)-[r:R*0..3]->(b) RETURN DISTINCT id(b)");
  std::unique_ptr<ir::QueryIR> query_ir = ir::CreateQueryIR(*statement);

  ir::LogicalPlanPtr plan = ir::CreateLogicalPlan(*query_ir);

  const ir::LogicalPlan *pruning =
      Find(*plan, ir::LogicalPlanNodeType::kPruningVarExpand);
  ASSERT_NE(pruning, nullptr);
  EXPECT_EQ(Find(*plan, ir::LogicalPlanNodeType::kVarExpand), nullptr);
  EXPECT_TRUE(pruning->EstimatedRows().has_value());
  EXPECT_TRUE(pruning->Cost().has_value());
}

TEST(LogicalPlanRewriterTest, DefaultPipelineLeavesPathSensitiveExpand) {
  std::unique_ptr<ast::Statement> statement = ast::ParseCypherAndRewrite(
      "MATCH (a)-[r:R*0..3]->(b) RETURN DISTINCT r, b");
  std::unique_ptr<ir::QueryIR> query_ir = ir::CreateQueryIR(*statement);

  ir::LogicalPlanPtr plan = ir::CreateLogicalPlan(*query_ir);

  EXPECT_NE(Find(*plan, ir::LogicalPlanNodeType::kVarExpand), nullptr);
  EXPECT_EQ(Find(*plan, ir::LogicalPlanNodeType::kPruningVarExpand), nullptr);
}
