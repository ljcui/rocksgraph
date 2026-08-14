#include "runtime/procedure_executor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "storage/in_memory_graph.h"

namespace {

std::unique_ptr<ir::ArgumentPlan> EmptySource() {
  return std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{});
}

ir::ProcedureYieldItem Yield(std::string field, std::string variable) {
  return {.result_field = std::move(field), .variable = std::move(variable)};
}

}  // namespace

TEST(ProcedureExecutorTest, PreservesInputRowsAndMapsYieldAliases) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"Person", "Employee"}, {{"name", rg::Value("Ada")}});
  rg::ProcedureExecutor executor(graph);
  ir::ProcedureCallPlan plan(EmptySource(), "db.labels", {},
                             {Yield("label", "label_name")}, false, true);

  rg::QueryRows rows = executor.Execute(
      plan, {{{"input", rg::Value(static_cast<std::int64_t>(7))}}});

  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].at("input").ToString(), "7");
  EXPECT_EQ(rows[0].at("label_name").ToString(), "\"Employee\"");
  EXPECT_EQ(rows[1].at("input").ToString(), "7");
  EXPECT_EQ(rows[1].at("label_name").ToString(), "\"Person\"");
}

TEST(ProcedureExecutorTest, RejectsPlansThatBypassSemanticValidation) {
  rg::InMemoryGraph graph;
  rg::ProcedureExecutor executor(graph);

  ir::ProcedureCallPlan unknown(EmptySource(), "db.unknown", {}, {}, false,
                                true);
  EXPECT_THROW((void)executor.Execute(unknown, {rg::QueryRow{}}),
               common::InvalidArgumentError);

  ir::ProcedureCallPlan with_argument(EmptySource(), "db.labels", {nullptr},
                                      {Yield("label", "label")}, false, true);
  EXPECT_THROW((void)executor.Execute(with_argument, {rg::QueryRow{}}),
               common::InvalidArgumentError);

  ir::ProcedureCallPlan unknown_yield(EmptySource(), "db.labels", {},
                                      {Yield("missing", "value")}, false, true);
  EXPECT_THROW((void)executor.Execute(unknown_yield, {rg::QueryRow{}}),
               common::InvalidArgumentError);

  ir::ProcedureCallPlan mismatched_mode(
      EmptySource(), "db.labels", {}, {Yield("label", "label")}, false, false);
  EXPECT_THROW((void)executor.Execute(mismatched_mode, {rg::QueryRow{}}),
               common::InvalidArgumentError);
}
