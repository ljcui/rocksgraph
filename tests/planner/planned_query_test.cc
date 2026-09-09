#include "planner/planned_query.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "ast/expression_to_string.h"
#include "ir/logical_plan_printer.h"

namespace {

ir::PlannedQuery PlanInNestedScope() {
  std::string cypher = "RETURN 1 + 2 AS value";
  return ir::PlanCypher(cypher);
}

}  // namespace

TEST(PlannedQueryTest, OwnsExpressionsAcrossMovesAndScopes) {
  ir::PlannedQuery query = PlanInNestedScope();
  ir::PlannedQuery moved = std::move(query);
  ir::PlannedQuery assigned = ir::PlanCypher("RETURN 0 AS discarded");
  assigned = std::move(moved);

  EXPECT_EQ(assigned.Ir().Kind(), ir::QueryIRKind::kSingle);
  EXPECT_NE(ir::LogicalPlanToString(assigned.Plan()).find("Projection [value]"),
            std::string::npos);
  const auto &projection =
      static_cast<const ir::ProjectionPlan &>(assigned.Plan().Child(0));
  ASSERT_EQ(projection.Items().size(), 1U);
  ASSERT_NE(projection.Items().front().expression, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*projection.Items().front().expression),
            "1 + 2");
}
