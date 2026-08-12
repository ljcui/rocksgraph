#include "runtime/query_executor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

namespace rg {
namespace {

using Rows = std::vector<QueryRow>;

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

const Value &LookupVariable(const QueryRow &row, const std::string &name) {
  const auto found = row.find(name);
  CHECK(found != row.end(), common::InvalidArgumentError,
        "variable is not bound: " + name);
  return found->second;
}

bool TryBind(QueryRow *row, const std::string &variable, Value value) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (variable.empty()) {
    return true;
  }
  const auto found = row->find(variable);
  if (found == row->end()) {
    row->emplace(variable, std::move(value));
    return true;
  }
  return ValuesEqual(found->second, value);
}

bool MergeRows(const QueryRow &left, const QueryRow &right, QueryRow *out) {
  CHECK(out != nullptr, common::InternalError, "query row is null");
  *out = left;
  for (const auto &[key, value] : right) {
    if (!TryBind(out, key, value)) {
      return false;
    }
  }
  return true;
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

bool RuntimeNodeHasLabels(const Node &node,
                          const std::vector<std::string> &labels) {
  for (const auto &label : labels) {
    if (std::find(node.labels.begin(), node.labels.end(), label) ==
        node.labels.end()) {
      return false;
    }
  }
  return true;
}

bool RuntimeRelationshipHasAnyType(const Relationship &relationship,
                                   const std::vector<std::string> &types) {
  return types.empty() || std::find(types.begin(), types.end(),
                                    relationship.type) != types.end();
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

class QueryExecutorImpl {
 public:
  QueryExecutorImpl(const AccessPath *access_path, Storage *storage)
      : access_path_(access_path), storage_(storage) {
    CHECK(access_path_ != nullptr, common::InvalidArgumentError,
          "access path is null");
  }

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
        return ExecuteAllNodeScan(
            static_cast<const ir::AllNodeScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeByLabelScan:
        return ExecuteNodeByLabelScan(
            static_cast<const ir::NodeByLabelScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeIndexSeek:
        return ExecuteNodeIndexSeek(
            static_cast<const ir::NodeIndexSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeIndexRangeSeek:
        return ExecuteNodeIndexRangeSeek(
            static_cast<const ir::NodeIndexRangeSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipTypeScan:
        return ExecuteRelationshipTypeScan(
            static_cast<const ir::RelationshipTypeScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipIndexSeek:
        return ExecuteRelationshipIndexSeek(
            static_cast<const ir::RelationshipIndexSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek:
        return ExecuteRelationshipIndexRangeSeek(
            static_cast<const ir::RelationshipIndexRangeSeekPlan &>(plan),
            input);
      case ir::LogicalPlanNodeType::kExpand:
        return ExecuteExpand(static_cast<const ir::ExpandPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kExpandInto:
        return ExecuteExpandInto(static_cast<const ir::ExpandIntoPlan &>(plan),
                                 input);
      case ir::LogicalPlanNodeType::kVarExpand:
        return ExecuteVarExpand(static_cast<const ir::VarExpandPlan &>(plan),
                                input);
      case ir::LogicalPlanNodeType::kPathBuild:
        return ExecutePathBuild(static_cast<const ir::PathBuildPlan &>(plan),
                                input);
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

  Rows ExecuteAllNodeScan(const ir::AllNodeScanPlan &plan, const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node : access_path_->ScanNodes()) {
        QueryRow next = row;
        if (TryBind(&next, plan.Variable(), Value(node))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteNodeByLabelScan(const ir::NodeByLabelScanPlan &plan,
                              const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node : access_path_->ScanNodes()) {
        if (!RuntimeNodeHasLabels(*node, plan.Labels())) {
          continue;
        }
        QueryRow next = row;
        if (TryBind(&next, plan.Variable(), Value(node))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteNodeIndexSeek(const ir::NodeIndexSeekPlan &plan,
                            const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      Value expected = EvaluateExpression(*plan.ValueExpression(), row);
      for (const auto &node : access_path_->FindNodesByIndex(
               plan.Labels(), plan.PropertyKey(), expected)) {
        QueryRow next = row;
        if (TryBind(&next, plan.Variable(), Value(node))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteNodeIndexRangeSeek(const ir::NodeIndexRangeSeekPlan &plan,
                                 const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node :
           access_path_->NodesInIndex(plan.Labels(), plan.PropertyKey())) {
        QueryRow next = row;
        if (!TryBind(&next, plan.Variable(), Value(node))) {
          continue;
        }
        bool keep = true;
        for (const ast::Expression *predicate : plan.Predicates()) {
          CHECK(predicate != nullptr, common::InvalidArgumentError,
                "index range predicate is null");
          keep = keep && PredicateIsTrue(EvaluateExpression(*predicate, next));
        }
        if (keep) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteRelationshipTypeScan(const ir::RelationshipTypeScanPlan &plan,
                                   const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &relationship : access_path_->ScanRelationships()) {
        if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        AddRelationshipRow(row, *relationship, plan.FromNode(),
                           plan.Relationship(), plan.ToNode(), plan.Direction(),
                           &out);
      }
    }
    return out;
  }

  Rows ExecuteRelationshipIndexSeek(const ir::RelationshipIndexSeekPlan &plan,
                                    const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      Value expected = EvaluateExpression(*plan.ValueExpression(), row);
      for (const auto &relationship : access_path_->FindRelationshipsByIndex(
               plan.Types(), plan.PropertyKey(), expected)) {
        AddRelationshipRow(row, *relationship, plan.FromNode(),
                           plan.Relationship(), plan.ToNode(), plan.Direction(),
                           &out);
      }
    }
    return out;
  }

  Rows ExecuteRelationshipIndexRangeSeek(
      const ir::RelationshipIndexRangeSeekPlan &plan, const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &relationship : access_path_->RelationshipsInIndex(
               plan.Types(), plan.PropertyKey())) {
        Rows candidate_rows;
        AddRelationshipRow(row, *relationship, plan.FromNode(),
                           plan.Relationship(), plan.ToNode(), plan.Direction(),
                           &candidate_rows);
        for (QueryRow &candidate : candidate_rows) {
          bool keep = true;
          for (const ast::Expression *predicate : plan.Predicates()) {
            CHECK(predicate != nullptr, common::InvalidArgumentError,
                  "index range predicate is null");
            keep = keep &&
                   PredicateIsTrue(EvaluateExpression(*predicate, candidate));
          }
          if (keep) {
            out.push_back(std::move(candidate));
          }
        }
      }
    }
    return out;
  }

  void AddRelationshipRow(const QueryRow &row, const Relationship &relationship,
                          const std::string &from_node,
                          const std::string &relationship_variable,
                          const std::string &to_node,
                          ir::ExpandDirection direction, Rows *out) {
    CHECK(out != nullptr, common::InternalError, "row output is null");
    if (direction == ir::ExpandDirection::kBoth) {
      AddDirectedRelationshipRow(
          row, relationship, from_node, relationship_variable, to_node,
          relationship.start_node_id, relationship.end_node_id, out);
      if (relationship.start_node_id != relationship.end_node_id) {
        AddDirectedRelationshipRow(
            row, relationship, from_node, relationship_variable, to_node,
            relationship.end_node_id, relationship.start_node_id, out);
      }
      return;
    }
    const std::int64_t from_id = direction == ir::ExpandDirection::kOutgoing
                                     ? relationship.start_node_id
                                     : relationship.end_node_id;
    const std::int64_t to_id = direction == ir::ExpandDirection::kOutgoing
                                   ? relationship.end_node_id
                                   : relationship.start_node_id;
    AddDirectedRelationshipRow(row, relationship, from_node,
                               relationship_variable, to_node, from_id, to_id,
                               out);
  }

  void AddDirectedRelationshipRow(const QueryRow &row,
                                  const Relationship &relationship,
                                  const std::string &from_node,
                                  const std::string &relationship_variable,
                                  const std::string &to_node,
                                  std::int64_t from_id, std::int64_t to_id,
                                  Rows *out) {
    QueryRow next = row;
    if (!TryBind(&next, from_node, Value(access_path_->NodeById(from_id)))) {
      return;
    }
    if (!TryBind(&next, relationship_variable,
                 Value(access_path_->RelationshipById(relationship.id)))) {
      return;
    }
    if (!TryBind(&next, to_node, Value(access_path_->NodeById(to_id)))) {
      return;
    }
    out->push_back(std::move(next));
  }

  Rows ExecuteExpand(const ir::ExpandPlan &plan, const Rows &input) {
    Rows child_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : child_rows) {
      const Value &from = LookupVariable(row, plan.FromNode());
      CHECK(from.IsNode(), common::InvalidArgumentError,
            "expand source is not a node: " + plan.FromNode());
      const std::int64_t from_id = from.AsNode().id;
      for (const auto &relationship :
           ExpandCandidates(from_id, plan.Direction())) {
        if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        if (plan.Direction() == ir::ExpandDirection::kOutgoing &&
            relationship->start_node_id != from_id) {
          continue;
        }
        if (plan.Direction() == ir::ExpandDirection::kIncoming &&
            relationship->end_node_id != from_id) {
          continue;
        }
        if (plan.Direction() == ir::ExpandDirection::kBoth &&
            relationship->start_node_id != from_id &&
            relationship->end_node_id != from_id) {
          continue;
        }
        const std::int64_t to_id = relationship->start_node_id == from_id
                                       ? relationship->end_node_id
                                       : relationship->start_node_id;
        QueryRow next = row;
        if (!TryBind(&next, plan.Relationship(),
                     Value(access_path_->RelationshipById(relationship->id)))) {
          continue;
        }
        if (!TryBind(&next, plan.ToNode(),
                     Value(access_path_->NodeById(to_id)))) {
          continue;
        }
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Rows ExecuteExpandInto(const ir::ExpandIntoPlan &plan, const Rows &input) {
    Rows child_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : child_rows) {
      const Value &from = LookupVariable(row, plan.FromNode());
      const Value &to = LookupVariable(row, plan.ToNode());
      CHECK(from.IsNode() && to.IsNode(), common::InvalidArgumentError,
            "expand-into endpoints must be nodes");
      for (const auto &relationship :
           ExpandCandidates(from.AsNode().id, plan.Direction())) {
        if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        bool match = false;
        if (plan.Direction() == ir::ExpandDirection::kOutgoing) {
          match = relationship->start_node_id == from.AsNode().id &&
                  relationship->end_node_id == to.AsNode().id;
        } else if (plan.Direction() == ir::ExpandDirection::kIncoming) {
          match = relationship->end_node_id == from.AsNode().id &&
                  relationship->start_node_id == to.AsNode().id;
        } else {
          match = (relationship->start_node_id == from.AsNode().id &&
                   relationship->end_node_id == to.AsNode().id) ||
                  (relationship->end_node_id == from.AsNode().id &&
                   relationship->start_node_id == to.AsNode().id);
        }
        if (!match) {
          continue;
        }
        QueryRow next = row;
        if (TryBind(&next, plan.Relationship(),
                    Value(access_path_->RelationshipById(relationship->id)))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  std::vector<AccessPath::RelationshipPtr> ExpandCandidates(
      std::int64_t from_node_id, ir::ExpandDirection direction) const {
    if (direction == ir::ExpandDirection::kOutgoing) {
      return access_path_->OutgoingRelationships(from_node_id);
    }
    if (direction == ir::ExpandDirection::kIncoming) {
      return access_path_->IncomingRelationships(from_node_id);
    }
    return access_path_->RelationshipsConnectedTo(from_node_id);
  }

  Rows ExecuteVarExpand(const ir::VarExpandPlan &plan, const Rows &input) {
    Rows child_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : child_rows) {
      const Value &from = LookupVariable(row, plan.FromNode());
      CHECK(from.IsNode(), common::InvalidArgumentError,
            "variable expand source is not a node: " + plan.FromNode());

      std::optional<std::int64_t> bound_to_id;
      const auto to_found = row.find(plan.ToNode());
      if (to_found != row.end()) {
        CHECK(to_found->second.IsNode(), common::InvalidArgumentError,
              "variable expand target is not a node: " + plan.ToNode());
        bound_to_id = to_found->second.AsNode().id;
      }

      const std::size_t min_length = VarExpandMinLength(plan.Length());
      const std::size_t max_length = VarExpandMaxLength(plan.Length());
      if (max_length < min_length) {
        continue;
      }

      std::vector<AccessPath::RelationshipPtr> path;
      std::unordered_set<std::int64_t> used_relationships;
      ExpandVariableLengthPath(plan, row, from.AsNode().id, bound_to_id,
                               min_length, max_length, &path,
                               &used_relationships, &out);
    }
    return out;
  }

  std::size_t VarExpandMinLength(
      const ir::LogicalVariableLength &length) const {
    if (!length.min.has_value()) {
      return 1;
    }
    CHECK(*length.min >= 0, common::InvalidArgumentError,
          "variable expand minimum length is negative");
    return static_cast<std::size_t>(*length.min);
  }

  std::size_t VarExpandMaxLength(
      const ir::LogicalVariableLength &length) const {
    if (!length.max.has_value()) {
      return access_path_->RelationshipCount();
    }
    CHECK(*length.max >= 0, common::InvalidArgumentError,
          "variable expand maximum length is negative");
    return static_cast<std::size_t>(*length.max);
  }

  std::optional<std::int64_t> NextVarExpandNode(
      const Relationship &relationship, std::int64_t current_node_id,
      ir::ExpandDirection direction) const {
    if (direction == ir::ExpandDirection::kOutgoing) {
      if (relationship.start_node_id != current_node_id) {
        return std::nullopt;
      }
      return relationship.end_node_id;
    }
    if (direction == ir::ExpandDirection::kIncoming) {
      if (relationship.end_node_id != current_node_id) {
        return std::nullopt;
      }
      return relationship.start_node_id;
    }
    if (relationship.start_node_id == current_node_id) {
      return relationship.end_node_id;
    }
    if (relationship.end_node_id == current_node_id) {
      return relationship.start_node_id;
    }
    return std::nullopt;
  }

  void ExpandVariableLengthPath(
      const ir::VarExpandPlan &plan, const QueryRow &row,
      std::int64_t current_node_id, std::optional<std::int64_t> bound_to_id,
      std::size_t min_length, std::size_t max_length,
      std::vector<AccessPath::RelationshipPtr> *path,
      std::unordered_set<std::int64_t> *used_relationships, Rows *out) {
    CHECK(path != nullptr, common::InternalError,
          "variable expand path is null");
    CHECK(used_relationships != nullptr, common::InternalError,
          "variable expand used relationship set is null");
    CHECK(out != nullptr, common::InternalError,
          "variable expand output is null");

    if (path->size() >= min_length &&
        (!bound_to_id.has_value() || *bound_to_id == current_node_id)) {
      EmitVarExpandRow(plan, row, current_node_id, *path, out);
    }
    if (path->size() == max_length) {
      return;
    }

    for (const auto &relationship :
         ExpandCandidates(current_node_id, plan.Direction())) {
      if (used_relationships->contains(relationship->id)) {
        continue;
      }
      if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
        continue;
      }
      std::optional<std::int64_t> next_node_id =
          NextVarExpandNode(*relationship, current_node_id, plan.Direction());
      if (!next_node_id.has_value()) {
        continue;
      }

      used_relationships->insert(relationship->id);
      path->push_back(relationship);
      ExpandVariableLengthPath(plan, row, *next_node_id, bound_to_id,
                               min_length, max_length, path, used_relationships,
                               out);
      path->pop_back();
      used_relationships->erase(relationship->id);
    }
  }

  void EmitVarExpandRow(const ir::VarExpandPlan &plan, const QueryRow &row,
                        std::int64_t current_node_id,
                        const std::vector<AccessPath::RelationshipPtr> &path,
                        Rows *out) {
    CHECK(out != nullptr, common::InternalError,
          "variable expand output is null");
    Value::List relationships;
    relationships.reserve(path.size());
    for (const auto &relationship : path) {
      CHECK(relationship != nullptr, common::InternalError,
            "variable expand relationship is null");
      relationships.emplace_back(relationship);
    }

    QueryRow next = row;
    if (!TryBind(&next, plan.Relationship(), Value(std::move(relationships)))) {
      return;
    }
    if (!TryBind(&next, plan.ToNode(),
                 Value(access_path_->NodeById(current_node_id)))) {
      return;
    }
    out->push_back(std::move(next));
  }

  Rows ExecutePathBuild(const ir::PathBuildPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    out.reserve(rows.size());
    for (const auto &row : rows) {
      QueryRow next = row;
      if (TryBind(&next, plan.PathVariable(),
                  BuildPathValue(plan.Path(), row))) {
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Value BuildPathValue(const ir::PathPattern &pattern, const QueryRow &row) {
    CHECK(!pattern.nodes.empty(), common::InvalidArgumentError,
          "path has no nodes: " + pattern.variable);
    CHECK(
        pattern.nodes.size() == pattern.relationships.size() + 1,
        common::InvalidArgumentError,
        "path node and relationship counts do not match: " + pattern.variable);

    auto path = std::make_shared<Path>();
    path->nodes.reserve(pattern.nodes.size());
    path->relationships.reserve(pattern.relationships.size());

    const Value &start_node = LookupVariable(row, pattern.nodes.front());
    CHECK(start_node.IsNode(), common::InvalidArgumentError,
          "path node is not a node: " + pattern.nodes.front());
    std::int64_t current_node_id = start_node.AsNode().id;
    path->nodes.push_back(access_path_->NodeById(current_node_id));

    for (std::size_t index = 0; index < pattern.relationships.size(); ++index) {
      const std::string &relationship_variable = pattern.relationships[index];
      const Value &relationship = LookupVariable(row, relationship_variable);
      std::vector<AccessPath::RelationshipPtr> relationships;
      if (relationship.IsList()) {
        relationships.reserve(relationship.AsList().size());
        for (const auto &item : relationship.AsList()) {
          CHECK(item.IsRelationship(), common::InvalidArgumentError,
                "path relationship list item is not a relationship: " +
                    relationship_variable);
          relationships.push_back(
              access_path_->RelationshipById(item.AsRelationship().id));
        }
      } else {
        CHECK(relationship.IsRelationship(), common::InvalidArgumentError,
              "path relationship is not a relationship: " +
                  relationship_variable);
        relationships.push_back(
            access_path_->RelationshipById(relationship.AsRelationship().id));
      }

      const Value &target_node = LookupVariable(row, pattern.nodes[index + 1]);
      CHECK(target_node.IsNode(), common::InvalidArgumentError,
            "path node is not a node: " + pattern.nodes[index + 1]);
      const std::int64_t target_node_id = target_node.AsNode().id;

      if (!CanTraverseRelationshipSequence(relationships, current_node_id,
                                           target_node_id)) {
        std::vector<AccessPath::RelationshipPtr> reversed(
            relationships.rbegin(), relationships.rend());
        CHECK(CanTraverseRelationshipSequence(reversed, current_node_id,
                                              target_node_id),
              common::InvalidArgumentError,
              "path relationship sequence does not connect nodes: " +
                  relationship_variable);
        relationships = std::move(reversed);
      }

      for (const auto &item : relationships) {
        AppendPathRelationship(*item, path.get(), &current_node_id);
      }
      CHECK(current_node_id == target_node_id, common::InvalidArgumentError,
            "path node does not match traversed relationship endpoint: " +
                pattern.nodes[index + 1]);
    }
    return Value(std::move(path));
  }

  bool CanTraverseRelationshipSequence(
      const std::vector<AccessPath::RelationshipPtr> &relationships,
      std::int64_t start_node_id, std::int64_t target_node_id) const {
    std::int64_t current_node_id = start_node_id;
    for (const auto &relationship : relationships) {
      CHECK(relationship != nullptr, common::InternalError,
            "path relationship is null");
      if (relationship->start_node_id == current_node_id) {
        current_node_id = relationship->end_node_id;
      } else if (relationship->end_node_id == current_node_id) {
        current_node_id = relationship->start_node_id;
      } else {
        return false;
      }
    }
    return current_node_id == target_node_id;
  }

  void AppendPathRelationship(const Relationship &relationship, Path *path,
                              std::int64_t *current_node_id) {
    CHECK(path != nullptr, common::InternalError, "path is null");
    CHECK(current_node_id != nullptr, common::InternalError,
          "path current node id is null");
    path->relationships.push_back(
        access_path_->RelationshipById(relationship.id));
    if (relationship.start_node_id == *current_node_id) {
      *current_node_id = relationship.end_node_id;
    } else if (relationship.end_node_id == *current_node_id) {
      *current_node_id = relationship.start_node_id;
    } else {
      THROW(common::InvalidArgumentError,
            "path relationship is not connected to current node");
    }
    path->nodes.push_back(access_path_->NodeById(*current_node_id));
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
      for (const auto &left : left_rows) {
        for (const auto &right : right_rows) {
          QueryRow merged;
          if (!MergeRows(left, right, &merged)) {
            continue;
          }
          if (!JoinPredicateMatches(plan, merged)) {
            continue;
          }
          out.push_back(std::move(merged));
        }
      }
    }
    return out;
  }

  bool JoinPredicateMatches(const ir::LogicalPlan &plan, const QueryRow &row) {
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kCartesianProduct:
        return true;
      case ir::LogicalPlanNodeType::kNodeHashJoin:
        return true;
      case ir::LogicalPlanNodeType::kValueHashJoin: {
        const auto &join = static_cast<const ir::ValueHashJoinPlan &>(plan);
        for (const ast::Expression *predicate : join.Predicates()) {
          if (!PredicateIsTrue(EvaluateExpression(*predicate, row))) {
            return false;
          }
        }
        return true;
      }
      case ir::LogicalPlanNodeType::kPredicateJoin: {
        const auto &join = static_cast<const ir::PredicateJoinPlan &>(plan);
        for (const ast::Expression *predicate : join.Predicates()) {
          if (!PredicateIsTrue(EvaluateExpression(*predicate, row))) {
            return false;
          }
        }
        return true;
      }
      default:
        break;
    }
    THROW(common::InternalError, "unknown join plan");
  }

  Rows ExecuteApply(const ir::LogicalPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      for (const auto &row : right_rows) {
        QueryRow merged;
        if (MergeRows(left, row, &merged)) {
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
        if (MergeRows(left, row, &merged)) {
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
      const Value &left = LookupVariable(row, plan.Relationship().left_node);
      const Value &right = LookupVariable(row, plan.Relationship().right_node);
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
          if (!MergeRows(row, match, &next)) {
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
        const Value &left = LookupVariable(row, relationship_pattern.left_node);
        const Value &right =
            LookupVariable(row, relationship_pattern.right_node);
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
