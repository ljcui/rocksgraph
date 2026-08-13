#include "runtime/apply_executor.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "value/value.h"

namespace {

std::unique_ptr<ir::ArgumentPlan> Arguments(std::vector<std::string> columns) {
  return std::make_unique<ir::ArgumentPlan>(std::move(columns));
}

const rg::Value &Column(const rg::QueryRow &row, const std::string &name) {
  return row.at(name);
}

rg::QueryRows LeftRows() {
  return {{{"left", rg::Value(1)}}, {{"left", rg::Value(2)}}};
}

template <typename Plan, typename RightExecutor>
rg::ApplyPlanExecutor CorrelatedExecutor(const Plan &plan,
                                         RightExecutor execute_right,
                                         std::size_t *right_calls = nullptr) {
  return [&plan, execute_right = std::move(execute_right), right_calls](
             const ir::LogicalPlan &child,
             const rg::QueryRows &input) -> rg::QueryRows {
    if (&child == &plan.Child(0)) {
      return LeftRows();
    }
    EXPECT_EQ(&child, &plan.Child(1));
    EXPECT_EQ(input.size(), 1U);
    if (right_calls != nullptr) {
      ++*right_calls;
    }
    return execute_right(input.front());
  };
}

}  // namespace

TEST(ApplyExecutorTest, ApplyExecutesRightPerLeftRowAndRejectsConflicts) {
  ir::ApplyPlan plan(Arguments({"left"}), Arguments({"right"}));
  std::size_t right_calls = 0;
  const rg::ApplyPlanExecutor execute_plan = CorrelatedExecutor(
      plan,
      [](const rg::QueryRow &left) {
        if (Column(left, "left") == rg::Value(1)) {
          return rg::QueryRows{{{"right", rg::Value("first")}},
                               {{"right", rg::Value("second")}}};
        }
        return rg::QueryRows{
            {{"left", rg::Value(99)}, {"right", rg::Value("conflict")}}};
      },
      &right_calls);

  rg::ApplyExecutor executor;
  const rg::QueryRows rows =
      executor.Execute(plan, {{{"seed", rg::Value(true)}}}, execute_plan);

  EXPECT_EQ(right_calls, 2U);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(Column(rows[0], "left"), rg::Value(1));
  EXPECT_EQ(Column(rows[0], "right"), rg::Value("first"));
  EXPECT_EQ(Column(rows[1], "right"), rg::Value("second"));
}

TEST(ApplyExecutorTest, SemiAndAntiApplyOnlyReturnLeftColumns) {
  auto execute_right = [](const rg::QueryRow &left) {
    if (Column(left, "left") == rg::Value(1)) {
      return rg::QueryRows{{{"right", rg::Value("match")}}};
    }
    return rg::QueryRows{};
  };
  rg::ApplyExecutor executor;

  ir::SemiApplyPlan semi_plan(Arguments({"left"}), Arguments({"right"}));
  const rg::QueryRows semi =
      executor.Execute(semi_plan, {rg::QueryRow{}},
                       CorrelatedExecutor(semi_plan, execute_right));
  ASSERT_EQ(semi.size(), 1U);
  EXPECT_EQ(Column(semi[0], "left"), rg::Value(1));
  EXPECT_FALSE(semi[0].contains("right"));

  ir::AntiSemiApplyPlan anti_plan(Arguments({"left"}), Arguments({"right"}));
  const rg::QueryRows anti =
      executor.Execute(anti_plan, {rg::QueryRow{}},
                       CorrelatedExecutor(anti_plan, execute_right));
  ASSERT_EQ(anti.size(), 1U);
  EXPECT_EQ(Column(anti[0], "left"), rg::Value(2));
  EXPECT_FALSE(anti[0].contains("right"));
}

TEST(ApplyExecutorTest, LetSemiApplyBindsExistenceForEveryLeftRow) {
  ir::LetSemiApplyPlan plan(Arguments({"left"}), Arguments({"right"}),
                            "exists");
  const rg::ApplyPlanExecutor execute_plan =
      CorrelatedExecutor(plan, [](const rg::QueryRow &left) {
        return Column(left, "left") == rg::Value(1)
                   ? rg::QueryRows{{{"right", rg::Value("match")}}}
                   : rg::QueryRows{};
      });

  rg::ApplyExecutor executor;
  const rg::QueryRows rows =
      executor.Execute(plan, {rg::QueryRow{}}, execute_plan);

  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(Column(rows[0], "exists"), rg::Value(true));
  EXPECT_EQ(Column(rows[1], "exists"), rg::Value(false));
  EXPECT_FALSE(rows[0].contains("right"));
}

TEST(ApplyExecutorTest, RollUpApplyPreservesDuplicatesAndProducesEmptyList) {
  ir::RollUpApplyPlan plan(Arguments({"left"}), Arguments({"item"}), "items",
                           "item");
  const rg::ApplyPlanExecutor execute_plan =
      CorrelatedExecutor(plan, [](const rg::QueryRow &left) {
        if (Column(left, "left") == rg::Value(1)) {
          return rg::QueryRows{{{"item", rg::Value("a")}},
                               {{"item", rg::Value("a")}},
                               {{"item", rg::Value("b")}}};
        }
        return rg::QueryRows{};
      });

  rg::ApplyExecutor executor;
  const rg::QueryRows rows =
      executor.Execute(plan, {rg::QueryRow{}}, execute_plan);

  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(Column(rows[0], "items"),
            rg::Value(rg::Value::List{rg::Value("a"), rg::Value("a"),
                                      rg::Value("b")}));
  EXPECT_EQ(Column(rows[1], "items"), rg::Value(rg::Value::List{}));
}

TEST(ApplyExecutorTest, RollUpApplyRejectsMissingValueVariable) {
  ir::RollUpApplyPlan plan(Arguments({"left"}), Arguments({"item"}), "items",
                           "item");
  const rg::ApplyPlanExecutor execute_plan =
      CorrelatedExecutor(plan, [](const rg::QueryRow &) {
        return rg::QueryRows{{{"other", rg::Value(1)}}};
      });

  rg::ApplyExecutor executor;
  EXPECT_THROW((void)executor.Execute(plan, {rg::QueryRow{}}, execute_plan),
               common::InvalidArgumentError);
}

TEST(ApplyExecutorTest, OptionalApplyMergesMatchesAndNullExtendsMisses) {
  ir::OptionalApplyPlan plan(Arguments({"left"}), Arguments({"left", "right"}));
  const rg::ApplyPlanExecutor execute_plan =
      CorrelatedExecutor(plan, [](const rg::QueryRow &left) {
        if (Column(left, "left") == rg::Value(1)) {
          return rg::QueryRows{
              {{"left", rg::Value(1.0)}, {"right", rg::Value("first")}},
              {{"left", rg::Value(1)}, {"right", rg::Value("second")}}};
        }
        return rg::QueryRows{};
      });

  rg::ApplyExecutor executor;
  const rg::QueryRows rows =
      executor.Execute(plan, {rg::QueryRow{}}, execute_plan);

  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(Column(rows[0], "right"), rg::Value("first"));
  EXPECT_EQ(Column(rows[1], "right"), rg::Value("second"));
  EXPECT_EQ(Column(rows[2], "left"), rg::Value(2));
  EXPECT_TRUE(Column(rows[2], "right").IsNull());
}

TEST(ApplyExecutorTest, RejectsEmptyPlanExecutor) {
  ir::ApplyPlan plan(Arguments({"left"}), Arguments({"right"}));
  rg::ApplyExecutor executor;

  EXPECT_THROW((void)executor.Execute(plan, {rg::QueryRow{}}, {}),
               common::InvalidArgumentError);
}
