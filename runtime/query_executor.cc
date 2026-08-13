#include "runtime/query_executor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_node.h"
#include "common/exception.h"
#include "ir/logical_plan_builder.h"
#include "ir/planner/catalog.h"
#include "ir/planner_query.h"
#include "runtime/aggregation_evaluator.h"
#include "runtime/expression_evaluator.h"
#include "runtime/graph_access_executor.h"
#include "runtime/join_executor.h"
#include "runtime/query_row_util.h"

namespace rg {
namespace {

using Rows = QueryRows;

struct EntityRef {
  enum class Kind { kNode, kRelationship };

  Kind kind = Kind::kNode;
  std::int64_t id = -1;
};

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

Value MakeValueFromLiteralMap(
    const std::vector<std::pair<std::string, const ast::Expression *>> &entries,
    const QueryRow &row) {
  Value::Map map;
  for (const auto &[key, expression] : entries) {
    CHECK(expression != nullptr, common::InvalidArgumentError,
          "map value is null");
    map[key] = EvaluateExpression(*expression, row);
  }
  return Value(std::move(map));
}

Value CopyOrNull(const Value *value) {
  return value != nullptr ? *value : Value::Null();
}

EntityRef MakeEntityRef(const Value &value) {
  CHECK(value.IsNode() || value.IsRelationship(), common::InvalidArgumentError,
        "expected node or relationship value");
  if (value.IsNode()) {
    return EntityRef{EntityRef::Kind::kNode, value.AsNode().id};
  }
  return EntityRef{EntityRef::Kind::kRelationship, value.AsRelationship().id};
}

Value::Map ParseMapLiteral(const ast::Expression &expression,
                           const QueryRow &row) {
  CHECK(expression.Is(ast::ASTNodeType::kMapLiteral),
        common::InvalidArgumentError, "expected map literal");
  const auto &map = ast::CastAst<ast::MapLiteral>(expression);
  Value::Map values;
  for (const auto &[key, value] : map.entries) {
    CHECK(value != nullptr, common::InvalidArgumentError, "map value is null");
    values[key] = EvaluateExpression(*value, row);
  }
  return values;
}

Value EnsureMapValue(const Value &value) {
  CHECK(value.IsMap(), common::InvalidArgumentError, "expected map value");
  return value;
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
  static const NoIndexPlannerCatalog catalog;
  return catalog;
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
        storage_(storage),
        graph_access_(RequireAccessPath(access_path)) {}

  QueryResult Execute(const ir::LogicalPlan &plan) {
    Rows rows = ExecutePlan(plan, Rows{QueryRow{}});
    return Materialize(plan.OutputColumns(), rows);
  }

  void ExecuteWrite(const ir::LogicalPlan &plan) {
    (void)ExecutePlan(plan, Rows{QueryRow{}});
  }

 private:
  QueryResult Materialize(const std::vector<std::string> &columns,
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
        return ExecuteSort(static_cast<const ir::SortPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSkip:
        return ExecuteSkip(static_cast<const ir::SkipPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kLimit:
        return ExecuteLimit(static_cast<const ir::LimitPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kProduceResults:
        return ExecutePlan(plan.Child(0), input);
      case ir::LogicalPlanNodeType::kCartesianProduct:
      case ir::LogicalPlanNodeType::kNodeHashJoin:
      case ir::LogicalPlanNodeType::kValueHashJoin:
      case ir::LogicalPlanNodeType::kPredicateJoin:
        return ExecuteJoin(plan, input);
      case ir::LogicalPlanNodeType::kApply:
        return ExecuteApply(plan, input);
      case ir::LogicalPlanNodeType::kSemiApply:
        return ExecuteSemiApply(plan, input);
      case ir::LogicalPlanNodeType::kAntiSemiApply:
        return ExecuteAntiSemiApply(plan, input);
      case ir::LogicalPlanNodeType::kLetSemiApply:
        return ExecuteLetSemiApply(
            static_cast<const ir::LetSemiApplyPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRollUpApply:
        return ExecuteRollUpApply(
            static_cast<const ir::RollUpApplyPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kOptionalApply:
        return ExecuteOptionalApply(plan, input);
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
        return ExecuteCreateNode(static_cast<const ir::CreateNodePlan &>(plan),
                                 input);
      case ir::LogicalPlanNodeType::kCreateRelationship:
        return ExecuteCreateRelationship(
            static_cast<const ir::CreateRelationshipPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kMerge:
        return ExecuteMerge(static_cast<const ir::MergePlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSetProperty:
        return ExecuteSetProperty(
            static_cast<const ir::SetPropertyPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSetProperties:
        return ExecuteSetProperties(
            static_cast<const ir::SetPropertiesPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSetLabels:
        return ExecuteSetLabels(static_cast<const ir::SetLabelsPlan &>(plan),
                                input);
      case ir::LogicalPlanNodeType::kRemoveProperty:
        return ExecuteRemoveProperty(
            static_cast<const ir::RemovePropertyPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRemoveLabels:
        return ExecuteRemoveLabels(
            static_cast<const ir::RemoveLabelsPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kDelete:
        return ExecuteDelete(static_cast<const ir::DeletePlan &>(plan), input,
                             false);
      case ir::LogicalPlanNodeType::kDetachDelete:
        return ExecuteDelete(static_cast<const ir::DetachDeletePlan &>(plan),
                             input, true);
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

  Rows ExecuteSort(const ir::SortPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    std::stable_sort(rows.begin(), rows.end(),
                     [&plan](const QueryRow &left, const QueryRow &right) {
                       for (const auto &item : plan.Items()) {
                         Value lhs = EvaluateLogicalSortItem(item, left);
                         Value rhs = EvaluateLogicalSortItem(item, right);
                         if (ValuesEqual(lhs, rhs)) {
                           continue;
                         }
                         const bool less = ValueLess(lhs, rhs);
                         return item.direction ==
                                        ir::LogicalOrderDirection::kAscending
                                    ? less
                                    : !less;
                       }
                       return false;
                     });
    return rows;
  }

  Rows ExecuteSkip(const ir::SkipPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    if (rows.empty()) {
      return rows;
    }
    const std::size_t skip = static_cast<std::size_t>(std::max<double>(
        0.0,
        EvaluateExpression(*plan.Skip(), rows[0], plan.PrecomputedExpressions())
            .AsInteger()));
    if (skip >= rows.size()) {
      return {};
    }
    return Rows(rows.begin() + static_cast<std::ptrdiff_t>(skip), rows.end());
  }

  Rows ExecuteLimit(const ir::LimitPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    if (rows.empty()) {
      return rows;
    }
    const std::size_t limit = static_cast<std::size_t>(
        std::max<double>(0.0, EvaluateExpression(*plan.Limit(), rows[0],
                                                 plan.PrecomputedExpressions())
                                  .AsInteger()));
    if (limit < rows.size()) {
      rows.resize(limit);
    }
    return rows;
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
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      for (const auto &row : right_rows) {
        QueryRow merged;
        if (MergeQueryRows(left, row, &merged)) {
          out.push_back(std::move(merged));
        }
      }
    }
    return out;
  }

  Rows ExecuteSemiApply(const ir::LogicalPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      if (!right_rows.empty()) {
        out.push_back(left);
      }
    }
    return out;
  }

  Rows ExecuteAntiSemiApply(const ir::LogicalPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      if (right_rows.empty()) {
        out.push_back(left);
      }
    }
    return out;
  }

  Rows ExecuteLetSemiApply(const ir::LetSemiApplyPlan &plan,
                           const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      QueryRow merged = left;
      merged[plan.ValueVariable()] = Value(!right_rows.empty());
      out.push_back(std::move(merged));
    }
    return out;
  }

  Rows ExecuteRollUpApply(const ir::RollUpApplyPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      QueryRow merged = left;
      Value::List list;
      for (const auto &row : right_rows) {
        const auto found = row.find(plan.ValueVariable());
        if (found != row.end()) {
          list.push_back(found->second);
        }
      }
      merged[plan.CollectionVariable()] = Value(std::move(list));
      out.push_back(std::move(merged));
    }
    return out;
  }

  Rows ExecuteOptionalApply(const ir::LogicalPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      if (right_rows.empty()) {
        QueryRow null_extended = left;
        for (const auto &column : plan.Child(1).OutputColumns()) {
          if (null_extended.find(column) == null_extended.end()) {
            null_extended[column] = Value::Null();
          }
        }
        out.push_back(std::move(null_extended));
        continue;
      }
      for (const auto &row : right_rows) {
        QueryRow merged;
        if (MergeQueryRows(left, row, &merged)) {
          out.push_back(std::move(merged));
        }
      }
    }
    return out;
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

  std::set<std::string> CollectLabels() const {
    std::set<std::string> labels;
    for (const auto &node : access_path_->ScanNodes()) {
      for (const auto &label : node->labels) {
        labels.insert(label);
      }
    }
    return labels;
  }

  std::set<std::string> CollectRelationshipTypes() const {
    std::set<std::string> types;
    for (const auto &relationship : access_path_->ScanRelationships()) {
      if (!relationship->type.empty()) {
        types.insert(relationship->type);
      }
    }
    return types;
  }

  std::set<std::string> CollectPropertyKeys() const {
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

  Rows ExecuteCreateNode(const ir::CreateNodePlan &plan, const Rows &input) {
    Storage &storage = WritableStorage();
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      Value::Map properties;
      for (const auto &entry : plan.Node().properties.entries) {
        CHECK(entry.value != nullptr, common::InvalidArgumentError,
              "CREATE node property value is null");
        properties[entry.key] = EvaluateExpression(*entry.value, row);
      }
      auto node = storage.CreateNode(plan.Node().labels, std::move(properties));
      QueryRow next = row;
      next[plan.Node().variable] = Value(node);
      out.push_back(std::move(next));
    }
    return out;
  }

  Rows ExecuteCreateRelationship(const ir::CreateRelationshipPlan &plan,
                                 const Rows &input) {
    Storage &storage = WritableStorage();
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      const Value &left =
          LookupQueryVariable(row, plan.Relationship().left_node);
      const Value &right =
          LookupQueryVariable(row, plan.Relationship().right_node);
      CHECK(left.IsNode() && right.IsNode(), common::InvalidArgumentError,
            "CREATE relationship endpoints must be nodes");
      Value::Map properties;
      for (const auto &entry : plan.Relationship().properties.entries) {
        CHECK(entry.value != nullptr, common::InvalidArgumentError,
              "CREATE relationship property value is null");
        properties[entry.key] = EvaluateExpression(*entry.value, row);
      }
      auto relationship = storage.CreateRelationship(
          left.AsNode().id, right.AsNode().id,
          plan.Relationship().types.empty() ? std::string()
                                            : plan.Relationship().types[0],
          std::move(properties));
      QueryRow next = row;
      next[plan.Relationship().variable] = Value(relationship);
      out.push_back(std::move(next));
    }
    return out;
  }

  Rows ExecuteMerge(const ir::MergePlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      Rows matches = ExecutePlan(plan.Child(1), Rows{row});
      if (!matches.empty()) {
        for (auto &match : matches) {
          QueryRow next;
          if (!MergeQueryRows(row, match, &next)) {
            continue;
          }
          for (const auto &action : plan.Merge().actions) {
            if (action.on_match) {
              ExecuteSetPatterns(action.set_patterns, &next);
            }
          }
          out.push_back(std::move(next));
        }
        continue;
      }

      QueryRow next = row;
      { next = ExecuteMergeCreate(plan.Merge().create_pattern, next); }
      for (const auto &action : plan.Merge().actions) {
        if (!action.on_match) {
          ExecuteSetPatterns(action.set_patterns, &next);
        }
      }
      out.push_back(std::move(next));
    }
    return out;
  }

  QueryRow ExecuteMergeCreate(const ir::CreatePattern &pattern, QueryRow row) {
    Storage &storage = WritableStorage();
    for (const auto &command : pattern.commands) {
      if (command.kind == ir::CreateEntityKind::kNode) {
        const auto &node_pattern = pattern.nodes.at(command.index);
        Value::Map properties;
        for (const auto &entry : node_pattern.properties.entries) {
          CHECK(entry.value != nullptr, common::InvalidArgumentError,
                "MERGE node property value is null");
          properties[entry.key] = EvaluateExpression(*entry.value, row);
        }
        auto node =
            storage.CreateNode(node_pattern.labels, std::move(properties));
        row[node_pattern.variable] = Value(node);
      } else {
        const auto &relationship_pattern =
            pattern.relationships.at(command.index);
        const Value &left =
            LookupQueryVariable(row, relationship_pattern.left_node);
        const Value &right =
            LookupQueryVariable(row, relationship_pattern.right_node);
        CHECK(left.IsNode() && right.IsNode(), common::InvalidArgumentError,
              "MERGE relationship endpoints must be nodes");
        Value::Map properties;
        for (const auto &entry : relationship_pattern.properties.entries) {
          CHECK(entry.value != nullptr, common::InvalidArgumentError,
                "MERGE relationship property value is null");
          properties[entry.key] = EvaluateExpression(*entry.value, row);
        }
        auto relationship = storage.CreateRelationship(
            left.AsNode().id, right.AsNode().id,
            relationship_pattern.types.empty() ? std::string()
                                               : relationship_pattern.types[0],
            std::move(properties));
        row[relationship_pattern.variable] = Value(relationship);
      }
    }
    return row;
  }

  void ExecuteSetPatterns(const std::vector<ir::SetMutatingPattern> &patterns,
                          QueryRow *row) {
    CHECK(row != nullptr, common::InternalError, "query row is null");
    for (const auto &pattern : patterns) {
      switch (pattern.kind) {
        case ir::SetMutatingPatternKind::kSetProperty:
          ApplySetProperty(pattern, row);
          break;
        case ir::SetMutatingPatternKind::kSetExactPropertiesFromMap:
          ApplySetProperties(pattern, row, false);
          break;
        case ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap:
          ApplySetProperties(pattern, row, true);
          break;
        case ir::SetMutatingPatternKind::kSetLabels:
          ApplySetLabels(pattern, row);
          break;
      }
    }
  }

  void ApplySetProperty(const ir::SetMutatingPattern &pattern, QueryRow *row) {
    Storage &storage = WritableStorage();
    const Value &entity = EvaluateExpression(*pattern.entity, *row);
    Value value = EvaluateExpression(*pattern.value, *row);
    if (entity.IsNode()) {
      storage.SetNodeProperty(entity.AsNode().id, pattern.property_key,
                              std::move(value));
      return;
    }
    if (entity.IsRelationship()) {
      storage.SetRelationshipProperty(entity.AsRelationship().id,
                                      pattern.property_key, std::move(value));
      return;
    }
    THROW(common::InvalidArgumentError, "SET property target is not an entity");
  }

  void ApplySetProperties(const ir::SetMutatingPattern &pattern, QueryRow *row,
                          bool include_existing) {
    Storage &storage = WritableStorage();
    const Value &entity = EvaluateExpression(*pattern.entity, *row);
    Value map_value = EvaluateExpression(*pattern.value, *row);
    CHECK(map_value.IsMap(), common::InvalidArgumentError,
          "SET properties requires a map value");
    if (entity.IsNode()) {
      storage.SetNodeProperties(entity.AsNode().id,
                                std::move(map_value.AsMap()), include_existing);
      return;
    }
    THROW(common::InvalidArgumentError, "SET properties target is not a node");
  }

  void ApplySetLabels(const ir::SetMutatingPattern &pattern, QueryRow *row) {
    Storage &storage = WritableStorage();
    const Value &entity = EvaluateExpression(*pattern.entity, *row);
    if (!entity.IsNode()) {
      THROW(common::InvalidArgumentError, "SET labels target is not a node");
    }
    storage.SetLabels(entity.AsNode().id, pattern.labels);
  }

  Rows ExecuteSetProperty(const ir::SetPropertyPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      ir::SetMutatingPattern pattern;
      pattern.kind = ir::SetMutatingPatternKind::kSetProperty;
      pattern.entity = plan.Entity();
      pattern.property_key = plan.PropertyKey();
      pattern.value = plan.Value();
      ExecuteSetPatterns({pattern}, &row);
    }
    return rows;
  }

  Rows ExecuteSetProperties(const ir::SetPropertiesPlan &plan,
                            const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      ir::SetMutatingPattern pattern;
      pattern.kind =
          plan.IncludeExisting()
              ? ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap
              : ir::SetMutatingPatternKind::kSetExactPropertiesFromMap;
      pattern.entity = plan.Entity();
      pattern.value = plan.Value();
      ExecuteSetPatterns({pattern}, &row);
    }
    return rows;
  }

  Rows ExecuteSetLabels(const ir::SetLabelsPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      ir::SetMutatingPattern pattern;
      pattern.kind = ir::SetMutatingPatternKind::kSetLabels;
      pattern.entity = plan.Entity();
      pattern.labels = plan.Labels();
      ExecuteSetPatterns({pattern}, &row);
    }
    return rows;
  }

  Rows ExecuteRemoveProperty(const ir::RemovePropertyPlan &plan,
                             const Rows &input) {
    Storage &storage = WritableStorage();
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      const Value &entity = EvaluateExpression(*plan.Entity(), row);
      if (entity.IsNode()) {
        storage.RemoveNodeProperty(entity.AsNode().id, plan.PropertyKey());
      } else if (entity.IsRelationship()) {
        storage.RemoveRelationshipProperty(entity.AsRelationship().id,
                                           plan.PropertyKey());
      } else {
        THROW(common::InvalidArgumentError,
              "REMOVE property target is not an entity");
      }
    }
    return rows;
  }

  Rows ExecuteRemoveLabels(const ir::RemoveLabelsPlan &plan,
                           const Rows &input) {
    Storage &storage = WritableStorage();
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      const Value &entity = EvaluateExpression(*plan.Entity(), row);
      if (!entity.IsNode()) {
        THROW(common::InvalidArgumentError,
              "REMOVE labels target is not a node");
      }
      storage.RemoveLabels(entity.AsNode().id, plan.Labels());
    }
    return rows;
  }

  Rows ExecuteDelete(const ir::LogicalPlan &plan, const Rows &input,
                     bool detach) {
    Storage &storage = WritableStorage();
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (const auto &row : rows) {
      std::vector<EntityRef> entities;
      if (plan.Type() == ir::LogicalPlanNodeType::kDelete) {
        const auto &del = static_cast<const ir::DeletePlan &>(plan);
        for (const auto *expression : del.Expressions()) {
          CHECK(expression != nullptr, common::InvalidArgumentError,
                "DELETE expression is null");
          Value value = EvaluateExpression(*expression, row);
          entities.push_back(MakeEntityRef(value));
        }
      } else {
        const auto &del = static_cast<const ir::DetachDeletePlan &>(plan);
        for (const auto *expression : del.Expressions()) {
          CHECK(expression != nullptr, common::InvalidArgumentError,
                "DETACH DELETE expression is null");
          Value value = EvaluateExpression(*expression, row);
          entities.push_back(MakeEntityRef(value));
        }
        detach = true;
      }
      for (const auto &entity : entities) {
        if (entity.kind == EntityRef::Kind::kRelationship) {
          storage.DeleteRelationship(entity.id);
          continue;
        }
        if (detach) {
          for (const auto &relationship :
               storage.RelationshipsConnectedTo(entity.id)) {
            storage.DeleteRelationship(relationship->id);
          }
        } else {
          CHECK(storage.RelationshipsConnectedTo(entity.id).empty(),
                common::InvalidArgumentError,
                "DELETE node still has relationships");
        }
        storage.DeleteNode(entity.id);
      }
    }
    return rows;
  }

  Storage &WritableStorage() const {
    CHECK(storage_ != nullptr, common::InvalidArgumentError,
          "write execution requires storage");
    return *storage_;
  }

  const AccessPath *access_path_ = nullptr;
  Storage *storage_ = nullptr;
  GraphAccessExecutor graph_access_;
  JoinExecutor join_executor_;
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
