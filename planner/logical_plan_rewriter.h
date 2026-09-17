#pragma once

#include <memory>
#include <vector>

#include "ir/logical_plan.h"

namespace planner {

class LogicalPlanRewriteRule {
 public:
  LogicalPlanRewriteRule() = default;
  LogicalPlanRewriteRule(const LogicalPlanRewriteRule &) = delete;
  LogicalPlanRewriteRule &operator=(const LogicalPlanRewriteRule &) = delete;
  virtual ~LogicalPlanRewriteRule() = default;

  // A rule may replace the current node but must preserve its output columns
  // and solved symbols.
  [[nodiscard]] virtual bool Apply(ir::LogicalPlanPtr *plan) const = 0;
};

class LogicalPlanRewritePipeline {
 public:
  LogicalPlanRewritePipeline() = default;
  explicit LogicalPlanRewritePipeline(
      std::vector<std::unique_ptr<LogicalPlanRewriteRule>> rules);

  void Add(std::unique_ptr<LogicalPlanRewriteRule> rule);
  [[nodiscard]] ir::LogicalPlanPtr Run(ir::LogicalPlanPtr plan) const;

 private:
  std::vector<std::unique_ptr<LogicalPlanRewriteRule>> rules_;
};

[[nodiscard]] LogicalPlanRewritePipeline
MakeDefaultLogicalPlanRewritePipeline();
[[nodiscard]] ir::LogicalPlanPtr RewriteLogicalPlan(ir::LogicalPlanPtr plan);

}  // namespace planner
