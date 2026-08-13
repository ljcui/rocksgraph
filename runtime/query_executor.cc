#include "runtime/query_executor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
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
#include "runtime/aggregation_evaluator.h"
#include "runtime/apply_executor.h"
#include "runtime/expression_evaluator.h"
#include "runtime/graph_access_executor.h"
#include "runtime/join_executor.h"
#include "runtime/query_row_util.h"
#include "runtime/result_set_executor.h"
#include "runtime/write_executor.h"

namespace rg {
namespace {

using Rows = QueryRows;

std::string LowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::optional<double> EvaluateNumericOptional(const ast::Expression *expression,
                                              const QueryRow &row) {
  if (expression == nullptr) {
    return std::nullopt;
  }
  Value value = EvaluateExpression(*expression, row);
  if (value.IsInteger()) {
    return static_cast<double>(value.AsInteger());
  }
  if (value.IsDouble()) {
    return value.AsDouble();
  }
  return std::nullopt;
}

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
        return ExecuteArgument(static_cast<const ir::ArgumentPlan &>(plan),
                               input);
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
        return ExecuteFilter(static_cast<const ir::FilterPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kProjection:
        return ExecuteProjection(static_cast<const ir::ProjectionPlan &>(plan),
                                 input);
      case ir::LogicalPlanNodeType::kDistinct:
        return ExecuteDistinct(static_cast<const ir::DistinctPlan &>(plan),
                               input);
      case ir::LogicalPlanNodeType::kAggregation:
        return ExecuteAggregation(
            static_cast<const ir::AggregationPlan &>(plan), input);
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
        return ExecuteUnwind(static_cast<const ir::UnwindPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kProcedureCall:
        return ExecuteProcedureCall(
            static_cast<const ir::ProcedureCallPlan &>(plan), input);
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

  Rows ExecuteArgument(const ir::ArgumentPlan &plan, const Rows &input) {
    Rows out;
    out.reserve(input.size());
    for (const auto &row : input) {
      QueryRow projected;
      if (plan.OutputColumns().empty()) {
        projected = row;
      } else {
        for (const auto &column : plan.OutputColumns()) {
          const auto found = row.find(column);
          CHECK(found != row.end(), common::InvalidArgumentError,
                "argument variable is not bound: " + column);
          projected.emplace(column, found->second);
        }
      }
      out.push_back(std::move(projected));
    }
    return out;
  }

  Rows ExecuteFilter(const ir::FilterPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (auto &row : rows) {
      if (PredicateIsTrue(EvaluateExpression(*plan.Predicate(), row,
                                             plan.PrecomputedExpressions()))) {
        out.push_back(std::move(row));
      }
    }
    return out;
  }

  Rows ExecuteProjection(const ir::ProjectionPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    out.reserve(rows.size());
    for (const auto &row : rows) {
      QueryRow projected;
      for (const auto &item : plan.Items()) {
        if (item.passthrough) {
          const auto found = row.find(item.alias);
          CHECK(found != row.end(), common::InvalidArgumentError,
                "passthrough projection variable is not bound: " + item.alias);
          projected[item.alias] = found->second;
        } else {
          projected[item.alias] = EvaluateLogicalProjectionItem(item, row);
        }
      }
      out.push_back(std::move(projected));
    }
    return out;
  }

  Rows ExecuteDistinct(const ir::DistinctPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    return ProjectDistinctRows(plan.GroupingItems(), rows);
  }

  Rows ExecuteAggregation(const ir::AggregationPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    return AggregateRows(plan.GroupingItems(), plan.AggregationItems(), rows);
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

  Rows ExecuteUnwind(const ir::UnwindPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      Value list = EvaluateExpression(*plan.Expression(), row);
      if (!list.IsList()) {
        continue;
      }
      for (const auto &item : list.AsList()) {
        QueryRow next = row;
        next[plan.Alias()] = item;
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Rows ExecuteProcedureCall(const ir::ProcedureCallPlan &plan,
                            const Rows &input) {
    CHECK(plan.ReadOnly(), common::InvalidArgumentError,
          "write procedure calls are not supported");
    const std::string procedure_name = LowerAscii(plan.ProcedureName());
    if (procedure_name == "db.labels") {
      return ExecuteMetadataProcedure(plan, input, "label", CollectLabels());
    }
    if (procedure_name == "db.relationshiptypes") {
      return ExecuteMetadataProcedure(plan, input, "relationshipType",
                                      CollectRelationshipTypes());
    }
    if (procedure_name == "db.propertykeys") {
      return ExecuteMetadataProcedure(plan, input, "propertyKey",
                                      CollectPropertyKeys());
    }

    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      QueryRow next = row;
      for (const auto &item : plan.YieldItems()) {
        if (item.variable.empty()) {
          continue;
        }
        next[item.variable] = Value::Null();
      }
      out.push_back(std::move(next));
    }
    return out;
  }

  [[nodiscard]] std::set<std::string> CollectLabels() const {
    std::set<std::string> labels;
    for (const auto &node : access_path_->ScanNodes()) {
      for (const auto &label : node->labels) {
        labels.insert(label);
      }
    }
    return labels;
  }

  [[nodiscard]] std::set<std::string> CollectRelationshipTypes() const {
    std::set<std::string> types;
    for (const auto &relationship : access_path_->ScanRelationships()) {
      if (!relationship->type.empty()) {
        types.insert(relationship->type);
      }
    }
    return types;
  }

  [[nodiscard]] std::set<std::string> CollectPropertyKeys() const {
    std::set<std::string> keys;
    for (const auto &node : access_path_->ScanNodes()) {
      for (const auto &[key, value] : node->properties) {
        (void)value;
        keys.insert(key);
      }
    }
    for (const auto &relationship : access_path_->ScanRelationships()) {
      for (const auto &[key, value] : relationship->properties) {
        (void)value;
        keys.insert(key);
      }
    }
    return keys;
  }

  Rows ExecuteMetadataProcedure(const ir::ProcedureCallPlan &plan,
                                const Rows &input, std::string_view field_name,
                                const std::set<std::string> &values) {
    CHECK(plan.Arguments().empty(), common::InvalidArgumentError,
          plan.ProcedureName() + "() expects no arguments");

    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      for (const auto &value : values) {
        QueryRow next = row;
        for (const auto &item : plan.YieldItems()) {
          if (item.variable.empty()) {
            continue;
          }
          const std::string field = item.result_field.value_or(item.variable);
          CHECK(
              field == field_name, common::InvalidArgumentError,
              "unsupported " + plan.ProcedureName() + " yield field: " + field);
          next[item.variable] = Value(value);
        }
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Rows ExecuteUnion(const ir::UnionPlan &plan, const Rows &input) {
    Rows out;
    std::set<std::string> seen;

    auto append_rows = [&](const Rows &rows, bool left_side) {
      for (const auto &row : rows) {
        QueryRow mapped;
        std::string key;
        for (const auto &mapping : plan.Mappings()) {
          const std::string &source =
              left_side ? mapping.lhs_variable : mapping.rhs_variable;
          const auto found = row.find(source);
          CHECK(found != row.end(), common::InvalidArgumentError,
                "UNION source variable is not bound: " + source);
          mapped[mapping.output_variable] = found->second;
          if (!plan.All()) {
            key += mapping.output_variable;
            key += '=';
            key += ValueKey(found->second);
            key += '\n';
          }
        }
        if (plan.All() || seen.insert(std::move(key)).second) {
          out.push_back(std::move(mapped));
        }
      }
    };

    append_rows(ExecutePlan(plan.Child(0), input), true);
    append_rows(ExecutePlan(plan.Child(1), input), false);
    return out;
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
  ResultSetExecutor result_set_executor_;
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
