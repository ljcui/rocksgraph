#include "runtime/query_executor.h"

#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_node.h"
#include "common/exception.h"
#include "ir/logical_plan_builder.h"
#include "ir/planner/catalog.h"
#include "ir/planner_query.h"
#include "runtime/apply_executor.h"
#include "runtime/graph_access_executor.h"
#include "runtime/join_executor.h"
#include "runtime/procedure_executor.h"
#include "runtime/result_set_executor.h"
#include "runtime/row_operator_executor.h"
#include "runtime/write_executor.h"

namespace rg {
namespace {

using Rows = QueryRows;

class NoIndexPlannerCatalog final : public ir::PlannerCatalog {
 public:
  [[nodiscard]] std::optional<ir::NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override {
    (void)labels;
    (void)property_key;
    return std::nullopt;
  }

  [[nodiscard]] std::optional<ir::RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const override {
    (void)relationship_types;
    (void)property_key;
    return std::nullopt;
  }
};

const ir::PlannerCatalog &DefaultRuntimePlannerCatalog() {
  static const NoIndexPlannerCatalog kCatalog;
  return kCatalog;
}

ir::LogicalPlanBuilderOptions PlannerOptionsFor(const QueryOptions &options) {
  return ir::LogicalPlanBuilderOptions{
      .max_idp_candidates_per_relationship_count =
          options.max_idp_candidates_per_relationship_count,
      .planner_statistics = options.planner_statistics,
      .planner_catalog = options.planner_catalog != nullptr
                             ? options.planner_catalog
                             : &DefaultRuntimePlannerCatalog()};
}

const AccessPath &RequireAccessPath(const AccessPath *access_path) {
  CHECK(access_path != nullptr, common::InvalidArgumentError,
        "access path is null");
  return *access_path;
}

class QueryExecutorImpl {
 public:
  QueryExecutorImpl(const AccessPath *access_path, Storage *storage)
      : access_path_(access_path),
        graph_access_(RequireAccessPath(access_path)),
        procedure_executor_(RequireAccessPath(access_path)),
        write_executor_(storage) {}

  QueryResult Execute(const ir::LogicalPlan &plan) {
    Rows rows = ExecutePlan(plan, Rows{QueryRow{}});
    return Materialize(plan.OutputColumns(), rows);
  }

  void ExecuteWrite(const ir::LogicalPlan &plan) {
    (void)ExecutePlan(plan, Rows{QueryRow{}});
  }

 private:
  [[nodiscard]] QueryResult Materialize(const std::vector<std::string> &columns,
                                        const Rows &rows) const {
    QueryResult result;
    result.columns = columns;
    result.rows.reserve(rows.size());
    for (const auto &row : rows) {
      std::vector<Value> values;
      values.reserve(columns.size());
      for (const auto &column : columns) {
        const auto found = row.find(column);
        values.push_back(found == row.end() ? Value::Null() : found->second);
      }
      result.rows.push_back(std::move(values));
    }
    return result;
  }

  Rows ExecutePlan(const ir::LogicalPlan &plan, const Rows &input) {
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kArgument:
        return row_operator_executor_.Execute(
            static_cast<const ir::ArgumentPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kAllNodeScan:
        return graph_access_.ExecuteAllNodeScan(
            static_cast<const ir::AllNodeScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeByLabelScan:
        return graph_access_.ExecuteNodeByLabelScan(
            static_cast<const ir::NodeByLabelScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeIndexSeek:
        return graph_access_.ExecuteNodeIndexSeek(
            static_cast<const ir::NodeIndexSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeIndexRangeSeek:
        return graph_access_.ExecuteNodeIndexRangeSeek(
            static_cast<const ir::NodeIndexRangeSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipTypeScan:
        return graph_access_.ExecuteRelationshipTypeScan(
            static_cast<const ir::RelationshipTypeScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipIndexSeek:
        return graph_access_.ExecuteRelationshipIndexSeek(
            static_cast<const ir::RelationshipIndexSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek:
        return graph_access_.ExecuteRelationshipIndexRangeSeek(
            static_cast<const ir::RelationshipIndexRangeSeekPlan &>(plan),
            input);
      case ir::LogicalPlanNodeType::kExpand:
        return graph_access_.ExecuteExpand(
            static_cast<const ir::ExpandPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kExpandInto:
        return graph_access_.ExecuteExpandInto(
            static_cast<const ir::ExpandIntoPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kVarExpand:
        return graph_access_.ExecuteVarExpand(
            static_cast<const ir::VarExpandPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kPathBuild:
        return graph_access_.ExecutePathBuild(
            static_cast<const ir::PathBuildPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kFilter:
        return row_operator_executor_.Execute(
            static_cast<const ir::FilterPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kProjection:
        return row_operator_executor_.Execute(
            static_cast<const ir::ProjectionPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kDistinct:
        return row_operator_executor_.Execute(
            static_cast<const ir::DistinctPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kAggregation:
        return row_operator_executor_.Execute(
            static_cast<const ir::AggregationPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kSort:
        return result_set_executor_.Execute(
            static_cast<const ir::SortPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kSkip:
        return result_set_executor_.Execute(
            static_cast<const ir::SkipPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kLimit:
        return result_set_executor_.Execute(
            static_cast<const ir::LimitPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kProduceResults:
        return ExecutePlan(plan.Child(0), input);
      case ir::LogicalPlanNodeType::kCartesianProduct:
      case ir::LogicalPlanNodeType::kNodeHashJoin:
      case ir::LogicalPlanNodeType::kValueHashJoin:
      case ir::LogicalPlanNodeType::kPredicateJoin:
        return ExecuteJoin(plan, input);
      case ir::LogicalPlanNodeType::kApply:
      case ir::LogicalPlanNodeType::kSemiApply:
      case ir::LogicalPlanNodeType::kAntiSemiApply:
      case ir::LogicalPlanNodeType::kLetSemiApply:
      case ir::LogicalPlanNodeType::kRollUpApply:
      case ir::LogicalPlanNodeType::kOptionalApply:
        return ExecuteApply(plan, input);
      case ir::LogicalPlanNodeType::kUnwind:
        return row_operator_executor_.Execute(
            static_cast<const ir::UnwindPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kProcedureCall:
        return procedure_executor_.Execute(
            static_cast<const ir::ProcedureCallPlan &>(plan),
            ExecutePlan(plan.Child(0), input));
      case ir::LogicalPlanNodeType::kUnion:
        return ExecuteUnion(static_cast<const ir::UnionPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kWriteBarrier:
        return ExecutePlan(plan.Child(0), input);
      case ir::LogicalPlanNodeType::kCreateNode:
      case ir::LogicalPlanNodeType::kCreateRelationship:
      case ir::LogicalPlanNodeType::kMerge:
      case ir::LogicalPlanNodeType::kSetProperty:
      case ir::LogicalPlanNodeType::kSetProperties:
      case ir::LogicalPlanNodeType::kSetLabels:
      case ir::LogicalPlanNodeType::kRemoveProperty:
      case ir::LogicalPlanNodeType::kRemoveLabels:
      case ir::LogicalPlanNodeType::kDelete:
      case ir::LogicalPlanNodeType::kDetachDelete:
        return ExecuteWritePlan(plan, input);
      case ir::LogicalPlanNodeType::kAssertIsNode:
        return ExecutePlan(plan.Child(0), input);
      default:
        THROW(common::InvalidArgumentError,
              "unsupported logical plan in executor: " +
                  std::string(plan.Name()));
    }
  }

  Rows ExecuteJoin(const ir::LogicalPlan &plan, const Rows &input) {
    Rows out;
    for (const auto &base : input) {
      Rows left_rows = ExecutePlan(plan.Child(0), Rows{base});
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{base});
      Rows joined;
      switch (plan.Type()) {
        case ir::LogicalPlanNodeType::kCartesianProduct:
          joined = join_executor_.Execute(
              static_cast<const ir::CartesianProductPlan &>(plan), left_rows,
              right_rows);
          break;
        case ir::LogicalPlanNodeType::kNodeHashJoin:
          joined = join_executor_.Execute(
              static_cast<const ir::NodeHashJoinPlan &>(plan), left_rows,
              right_rows);
          break;
        case ir::LogicalPlanNodeType::kValueHashJoin:
          joined = join_executor_.Execute(
              static_cast<const ir::ValueHashJoinPlan &>(plan), left_rows,
              right_rows);
          break;
        case ir::LogicalPlanNodeType::kPredicateJoin:
          joined = join_executor_.Execute(
              static_cast<const ir::PredicateJoinPlan &>(plan), left_rows,
              right_rows);
          break;
        default:
          THROW(common::InternalError, "unknown join plan");
      }
      out.insert(out.end(), std::make_move_iterator(joined.begin()),
                 std::make_move_iterator(joined.end()));
    }
    return out;
  }

  Rows ExecuteApply(const ir::LogicalPlan &plan, const Rows &input) {
    const ApplyPlanExecutor execute_plan =
        [this](const ir::LogicalPlan &child, const QueryRows &child_input) {
          return ExecutePlan(child, child_input);
        };
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kApply:
        return apply_executor_.Execute(static_cast<const ir::ApplyPlan &>(plan),
                                       input, execute_plan);
      case ir::LogicalPlanNodeType::kSemiApply:
        return apply_executor_.Execute(
            static_cast<const ir::SemiApplyPlan &>(plan), input, execute_plan);
      case ir::LogicalPlanNodeType::kAntiSemiApply:
        return apply_executor_.Execute(
            static_cast<const ir::AntiSemiApplyPlan &>(plan), input,
            execute_plan);
      case ir::LogicalPlanNodeType::kLetSemiApply:
        return apply_executor_.Execute(
            static_cast<const ir::LetSemiApplyPlan &>(plan), input,
            execute_plan);
      case ir::LogicalPlanNodeType::kRollUpApply:
        return apply_executor_.Execute(
            static_cast<const ir::RollUpApplyPlan &>(plan), input,
            execute_plan);
      case ir::LogicalPlanNodeType::kOptionalApply:
        return apply_executor_.Execute(
            static_cast<const ir::OptionalApplyPlan &>(plan), input,
            execute_plan);
      default:
        THROW(common::InternalError, "unknown apply plan");
    }
  }

  Rows ExecuteUnion(const ir::UnionPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows right_rows = ExecutePlan(plan.Child(1), input);
    return row_operator_executor_.Execute(plan, left_rows, right_rows);
  }

  Rows ExecuteWritePlan(const ir::LogicalPlan &plan, const Rows &input) {
    const WritePlanExecutor execute_plan =
        [this](const ir::LogicalPlan &child, const QueryRows &child_input) {
          return ExecutePlan(child, child_input);
        };
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kCreateNode:
        return write_executor_.Execute(
            static_cast<const ir::CreateNodePlan &>(plan), input, execute_plan);
      case ir::LogicalPlanNodeType::kCreateRelationship:
        return write_executor_.Execute(
            static_cast<const ir::CreateRelationshipPlan &>(plan), input,
            execute_plan);
      case ir::LogicalPlanNodeType::kMerge:
        return write_executor_.Execute(static_cast<const ir::MergePlan &>(plan),
                                       input, execute_plan);
      case ir::LogicalPlanNodeType::kSetProperty:
        return write_executor_.Execute(
            static_cast<const ir::SetPropertyPlan &>(plan), input,
            execute_plan);
      case ir::LogicalPlanNodeType::kSetProperties:
        return write_executor_.Execute(
            static_cast<const ir::SetPropertiesPlan &>(plan), input,
            execute_plan);
      case ir::LogicalPlanNodeType::kSetLabels:
        return write_executor_.Execute(
            static_cast<const ir::SetLabelsPlan &>(plan), input, execute_plan);
      case ir::LogicalPlanNodeType::kRemoveProperty:
        return write_executor_.Execute(
            static_cast<const ir::RemovePropertyPlan &>(plan), input,
            execute_plan);
      case ir::LogicalPlanNodeType::kRemoveLabels:
        return write_executor_.Execute(
            static_cast<const ir::RemoveLabelsPlan &>(plan), input,
            execute_plan);
      case ir::LogicalPlanNodeType::kDelete:
        return write_executor_.Execute(
            static_cast<const ir::DeletePlan &>(plan), input, execute_plan);
      case ir::LogicalPlanNodeType::kDetachDelete:
        return write_executor_.Execute(
            static_cast<const ir::DetachDeletePlan &>(plan), input,
            execute_plan);
      default:
        THROW(common::InternalError, "unknown write plan");
    }
  }

  const AccessPath *access_path_ = nullptr;
  GraphAccessExecutor graph_access_;
  JoinExecutor join_executor_;
  ApplyExecutor apply_executor_;
  ProcedureExecutor procedure_executor_;
  ResultSetExecutor result_set_executor_;
  RowOperatorExecutor row_operator_executor_;
  WriteExecutor write_executor_;
};

}  // namespace

QueryResult QueryExecutor::Execute(const ir::LogicalPlan &plan) const {
  CHECK(access_path_ != nullptr, common::InternalError, "access path is null");
  return QueryExecutorImpl(access_path_, storage_).Execute(plan);
}

void QueryExecutor::ExecuteWrite(const ir::LogicalPlan &plan) {
  CHECK(storage_ != nullptr, common::InternalError,
        "write execution requires storage");
  QueryExecutorImpl(storage_, storage_).ExecuteWrite(plan);
}

QueryResult ExecuteReadQuery(const AccessPath &access_path,
                             std::string_view cypher, QueryOptions options) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan =
      ir::CreateLogicalPlan(*planner_query, PlannerOptionsFor(options));
  return QueryExecutor(access_path).Execute(*logical_plan);
}

QueryResult ExecuteQuery(Storage &storage, std::string_view cypher,
                         QueryOptions options) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan =
      ir::CreateLogicalPlan(*planner_query, PlannerOptionsFor(options));
  return QueryExecutor(storage).Execute(*logical_plan);
}

void ExecuteWriteQuery(Storage &storage, std::string_view cypher,
                       QueryOptions options) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan =
      ir::CreateLogicalPlan(*planner_query, PlannerOptionsFor(options));
  QueryExecutor(storage).ExecuteWrite(*logical_plan);
}

}  // namespace rg
