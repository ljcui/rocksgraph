#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ir/planner_query.h"

namespace ast {
class Expression;
}  // namespace ast

namespace ir {

class LogicalPlan;
using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

enum class LogicalPlanNodeType {
  kArgument,
  kAllNodeScan,
  kNodeByLabelScan,
  kNodeIndexSeek,
  kNodeIndexRangeSeek,
  kRelationshipTypeScan,
  kRelationshipIndexSeek,
  kRelationshipIndexRangeSeek,
  kExpand,
  kExpandInto,
  kVarExpand,
  kPathBuild,
  kFilter,
  kProjection,
  kDistinct,
  kAggregation,
  kSort,
  kSkip,
  kLimit,
  kProduceResults,
  kCartesianProduct,
  kNodeHashJoin,
  kValueHashJoin,
  kPredicateJoin,
  kApply,
  kSemiApply,
  kAntiSemiApply,
  kLetSemiApply,
  kRollUpApply,
  kOptionalApply,
  kAssertIsNode,
  kWriteBarrier,
  kCreateNode,
  kCreateRelationship,
  kMerge,
  kSetProperty,
  kSetProperties,
  kSetLabels,
  kRemoveProperty,
  kRemoveLabels,
  kDelete,
  kDetachDelete,
  kUnwind,
  kProcedureCall,
  kUnion,
};

enum class ExpandDirection {
  kIncoming,
  kOutgoing,
  kBoth,
};

[[nodiscard]] std::string_view ToString(LogicalPlanNodeType type);
[[nodiscard]] std::string_view ToString(ExpandDirection direction);

enum class LogicalOrderDirection {
  kAscending,
  kDescending,
};

[[nodiscard]] std::string_view ToString(LogicalOrderDirection direction);

struct LogicalPrecomputedExpression {
  const ast::Expression *expression = nullptr;
  std::string variable;
};

struct LogicalProjectionItem {
  const ast::Expression *expression = nullptr;
  std::string alias;
  std::vector<LogicalPrecomputedExpression> precomputed_expressions;
  bool passthrough = false;
};

struct LogicalSortItem {
  const ast::Expression *expression = nullptr;
  LogicalOrderDirection direction = LogicalOrderDirection::kAscending;
  std::vector<LogicalPrecomputedExpression> precomputed_expressions;
};

struct LogicalPlanMetadata {
  std::optional<double> estimated_rows;
  std::optional<double> cost;
  std::vector<LogicalSortItem> ordering;
  bool distinct = false;
};

struct LogicalUnionMapping {
  std::string output_variable;
  std::string lhs_variable;
  std::string rhs_variable;
};

struct LogicalVariableLength {
  std::optional<int> min;
  std::optional<int> max;
};

class LogicalPlan {
 public:
  LogicalPlan(const LogicalPlan &) = delete;
  LogicalPlan &operator=(const LogicalPlan &) = delete;
  LogicalPlan(LogicalPlan &&) noexcept = default;
  LogicalPlan &operator=(LogicalPlan &&) noexcept = default;
  virtual ~LogicalPlan();

  [[nodiscard]] LogicalPlanNodeType Type() const noexcept { return type_; }
  [[nodiscard]] std::string_view Name() const { return ToString(type_); }
  [[nodiscard]] virtual std::string Details() const;

  [[nodiscard]] const std::unordered_set<std::string> &SolvedSymbols()
      const noexcept {
    return solved_symbols_;
  }
  [[nodiscard]] const std::vector<std::string> &OutputColumns() const noexcept {
    return output_columns_;
  }
  [[nodiscard]] const LogicalPlanMetadata &Metadata() const noexcept {
    return metadata_;
  }
  [[nodiscard]] const std::optional<double> &EstimatedRows() const noexcept {
    return metadata_.estimated_rows;
  }
  [[nodiscard]] const std::optional<double> &Cost() const noexcept {
    return metadata_.cost;
  }
  [[nodiscard]] const std::vector<LogicalSortItem> &OrderingTrait()
      const noexcept {
    return metadata_.ordering;
  }
  [[nodiscard]] bool DistinctTrait() const noexcept {
    return metadata_.distinct;
  }
  [[nodiscard]] const std::vector<LogicalPlanPtr> &Children() const noexcept {
    return children_;
  }
  [[nodiscard]] std::size_t ChildCount() const noexcept {
    return children_.size();
  }
  [[nodiscard]] const LogicalPlan &Child(std::size_t index) const;
  [[nodiscard]] LogicalPlan &Child(std::size_t index);

  void SetCostEstimate(double estimated_rows, double cost);
  void ClearCostEstimate();
  void SetOrderingTrait(std::vector<LogicalSortItem> ordering);
  void ClearOrderingTrait();
  void SetDistinctTrait(bool distinct);
  void CopyMetadataFrom(const LogicalPlan &source);

 protected:
  explicit LogicalPlan(LogicalPlanNodeType type,
                       std::vector<LogicalPlanPtr> children = {});

  void SetSolvedSymbols(std::unordered_set<std::string> symbols);
  void SetOutputColumns(std::vector<std::string> columns);
  void AddSolvedSymbol(std::string_view symbol);
  void AddOutputColumn(std::string_view column);

 private:
  LogicalPlanNodeType type_;
  std::unordered_set<std::string> solved_symbols_;
  std::vector<std::string> output_columns_;
  LogicalPlanMetadata metadata_;
  std::vector<LogicalPlanPtr> children_;
};

class ArgumentPlan final : public LogicalPlan {
 public:
  explicit ArgumentPlan(std::vector<std::string> symbols);

  [[nodiscard]] std::string Details() const override;
};

class AllNodeScanPlan final : public LogicalPlan {
 public:
  explicit AllNodeScanPlan(std::string variable);

  [[nodiscard]] const std::string &Variable() const noexcept {
    return variable_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string variable_;
};

class NodeByLabelScanPlan final : public LogicalPlan {
 public:
  NodeByLabelScanPlan(std::string variable, std::string label);
  NodeByLabelScanPlan(std::string variable, std::vector<std::string> labels);

  [[nodiscard]] const std::string &Variable() const noexcept {
    return variable_;
  }
  [[nodiscard]] const std::string &Label() const noexcept;
  [[nodiscard]] const std::vector<std::string> &Labels() const noexcept {
    return labels_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string variable_;
  std::vector<std::string> labels_;
};

class NodeIndexSeekPlan final : public LogicalPlan {
 public:
  NodeIndexSeekPlan(std::string variable, std::vector<std::string> labels,
                    std::string property_key,
                    const ast::Expression *value_expression,
                    bool unique = false);

  [[nodiscard]] const std::string &Variable() const noexcept {
    return variable_;
  }
  [[nodiscard]] const std::vector<std::string> &Labels() const noexcept {
    return labels_;
  }
  [[nodiscard]] const std::string &PropertyKey() const noexcept {
    return property_key_;
  }
  [[nodiscard]] const ast::Expression *ValueExpression() const noexcept {
    return value_expression_;
  }
  [[nodiscard]] bool Unique() const noexcept { return unique_; }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string variable_;
  std::vector<std::string> labels_;
  std::string property_key_;
  const ast::Expression *value_expression_ = nullptr;
  bool unique_ = false;
};

class NodeIndexRangeSeekPlan final : public LogicalPlan {
 public:
  NodeIndexRangeSeekPlan(std::string variable, std::vector<std::string> labels,
                         std::string property_key,
                         std::vector<const ast::Expression *> predicates);

  [[nodiscard]] const std::string &Variable() const noexcept {
    return variable_;
  }
  [[nodiscard]] const std::vector<std::string> &Labels() const noexcept {
    return labels_;
  }
  [[nodiscard]] const std::string &PropertyKey() const noexcept {
    return property_key_;
  }
  [[nodiscard]] const std::vector<const ast::Expression *> &Predicates()
      const noexcept {
    return predicates_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string variable_;
  std::vector<std::string> labels_;
  std::string property_key_;
  std::vector<const ast::Expression *> predicates_;
};

class RelationshipTypeScanPlan final : public LogicalPlan {
 public:
  RelationshipTypeScanPlan(std::string from_node, std::string relationship,
                           std::string to_node, ExpandDirection direction,
                           std::vector<std::string> types);

  [[nodiscard]] const std::string &FromNode() const noexcept {
    return from_node_;
  }
  [[nodiscard]] const std::string &Relationship() const noexcept {
    return relationship_;
  }
  [[nodiscard]] const std::string &ToNode() const noexcept { return to_node_; }
  [[nodiscard]] ExpandDirection Direction() const noexcept {
    return direction_;
  }
  [[nodiscard]] const std::vector<std::string> &Types() const noexcept {
    return types_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string from_node_;
  std::string relationship_;
  std::string to_node_;
  ExpandDirection direction_ = ExpandDirection::kBoth;
  std::vector<std::string> types_;
};

class RelationshipIndexSeekPlan final : public LogicalPlan {
 public:
  RelationshipIndexSeekPlan(std::string from_node, std::string relationship,
                            std::string to_node, ExpandDirection direction,
                            std::vector<std::string> types,
                            std::string property_key,
                            const ast::Expression *value_expression,
                            bool unique = false);

  [[nodiscard]] const std::string &FromNode() const noexcept {
    return from_node_;
  }
  [[nodiscard]] const std::string &Relationship() const noexcept {
    return relationship_;
  }
  [[nodiscard]] const std::string &ToNode() const noexcept { return to_node_; }
  [[nodiscard]] ExpandDirection Direction() const noexcept {
    return direction_;
  }
  [[nodiscard]] const std::vector<std::string> &Types() const noexcept {
    return types_;
  }
  [[nodiscard]] const std::string &PropertyKey() const noexcept {
    return property_key_;
  }
  [[nodiscard]] const ast::Expression *ValueExpression() const noexcept {
    return value_expression_;
  }
  [[nodiscard]] bool Unique() const noexcept { return unique_; }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string from_node_;
  std::string relationship_;
  std::string to_node_;
  ExpandDirection direction_ = ExpandDirection::kBoth;
  std::vector<std::string> types_;
  std::string property_key_;
  const ast::Expression *value_expression_ = nullptr;
  bool unique_ = false;
};

class RelationshipIndexRangeSeekPlan final : public LogicalPlan {
 public:
  RelationshipIndexRangeSeekPlan(
      std::string from_node, std::string relationship, std::string to_node,
      ExpandDirection direction, std::vector<std::string> types,
      std::string property_key,
      std::vector<const ast::Expression *> predicates);

  [[nodiscard]] const std::string &FromNode() const noexcept {
    return from_node_;
  }
  [[nodiscard]] const std::string &Relationship() const noexcept {
    return relationship_;
  }
  [[nodiscard]] const std::string &ToNode() const noexcept { return to_node_; }
  [[nodiscard]] ExpandDirection Direction() const noexcept {
    return direction_;
  }
  [[nodiscard]] const std::vector<std::string> &Types() const noexcept {
    return types_;
  }
  [[nodiscard]] const std::string &PropertyKey() const noexcept {
    return property_key_;
  }
  [[nodiscard]] const std::vector<const ast::Expression *> &Predicates()
      const noexcept {
    return predicates_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string from_node_;
  std::string relationship_;
  std::string to_node_;
  ExpandDirection direction_ = ExpandDirection::kBoth;
  std::vector<std::string> types_;
  std::string property_key_;
  std::vector<const ast::Expression *> predicates_;
};

class ExpandPlan final : public LogicalPlan {
 public:
  ExpandPlan(LogicalPlanPtr source, std::string from_node,
             std::string relationship, std::string to_node,
             ExpandDirection direction, std::vector<std::string> types = {});

  [[nodiscard]] const std::string &FromNode() const noexcept {
    return from_node_;
  }
  [[nodiscard]] const std::string &Relationship() const noexcept {
    return relationship_;
  }
  [[nodiscard]] const std::string &ToNode() const noexcept { return to_node_; }
  [[nodiscard]] ExpandDirection Direction() const noexcept {
    return direction_;
  }
  [[nodiscard]] const std::vector<std::string> &Types() const noexcept {
    return types_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string from_node_;
  std::string relationship_;
  std::string to_node_;
  ExpandDirection direction_ = ExpandDirection::kBoth;
  std::vector<std::string> types_;
};

class ExpandIntoPlan final : public LogicalPlan {
 public:
  ExpandIntoPlan(LogicalPlanPtr source, std::string from_node,
                 std::string relationship, std::string to_node,
                 ExpandDirection direction,
                 std::vector<std::string> types = {});

  [[nodiscard]] const std::string &FromNode() const noexcept {
    return from_node_;
  }
  [[nodiscard]] const std::string &Relationship() const noexcept {
    return relationship_;
  }
  [[nodiscard]] const std::string &ToNode() const noexcept { return to_node_; }
  [[nodiscard]] ExpandDirection Direction() const noexcept {
    return direction_;
  }
  [[nodiscard]] const std::vector<std::string> &Types() const noexcept {
    return types_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string from_node_;
  std::string relationship_;
  std::string to_node_;
  ExpandDirection direction_ = ExpandDirection::kBoth;
  std::vector<std::string> types_;
};

class VarExpandPlan final : public LogicalPlan {
 public:
  VarExpandPlan(LogicalPlanPtr source, std::string from_node,
                std::string relationship, std::string to_node,
                ExpandDirection direction, std::vector<std::string> types,
                LogicalVariableLength length);

  [[nodiscard]] const std::string &FromNode() const noexcept {
    return from_node_;
  }
  [[nodiscard]] const std::string &Relationship() const noexcept {
    return relationship_;
  }
  [[nodiscard]] const std::string &ToNode() const noexcept { return to_node_; }
  [[nodiscard]] ExpandDirection Direction() const noexcept {
    return direction_;
  }
  [[nodiscard]] const std::vector<std::string> &Types() const noexcept {
    return types_;
  }
  [[nodiscard]] const LogicalVariableLength &Length() const noexcept {
    return length_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string from_node_;
  std::string relationship_;
  std::string to_node_;
  ExpandDirection direction_ = ExpandDirection::kBoth;
  std::vector<std::string> types_;
  LogicalVariableLength length_;
};

class PathBuildPlan final : public LogicalPlan {
 public:
  PathBuildPlan(LogicalPlanPtr source, PathPattern path);

  [[nodiscard]] const std::string &PathVariable() const noexcept {
    return path_.variable;
  }
  [[nodiscard]] const PathPattern &Path() const noexcept { return path_; }
  [[nodiscard]] std::string Details() const override;

 private:
  PathPattern path_;
};

class FilterPlan final : public LogicalPlan {
 public:
  FilterPlan(LogicalPlanPtr source, const ast::Expression *predicate);
  FilterPlan(LogicalPlanPtr source, const ast::Expression *predicate,
             std::vector<LogicalPrecomputedExpression> precomputed_expressions);

  [[nodiscard]] const ast::Expression *Predicate() const noexcept {
    return predicate_;
  }
  [[nodiscard]] const std::vector<LogicalPrecomputedExpression> &
  PrecomputedExpressions() const noexcept {
    return precomputed_expressions_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *predicate_ = nullptr;
  std::vector<LogicalPrecomputedExpression> precomputed_expressions_;
};

class ProjectionPlan final : public LogicalPlan {
 public:
  ProjectionPlan(LogicalPlanPtr source,
                 std::vector<LogicalProjectionItem> items);

  [[nodiscard]] const std::vector<LogicalProjectionItem> &Items()
      const noexcept {
    return items_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<LogicalProjectionItem> items_;
};

class DistinctPlan final : public LogicalPlan {
 public:
  DistinctPlan(LogicalPlanPtr source,
               std::vector<LogicalProjectionItem> grouping_items);

  [[nodiscard]] const std::vector<LogicalProjectionItem> &GroupingItems()
      const noexcept {
    return grouping_items_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<LogicalProjectionItem> grouping_items_;
};

class AggregationPlan final : public LogicalPlan {
 public:
  AggregationPlan(LogicalPlanPtr source,
                  std::vector<LogicalProjectionItem> grouping_items,
                  std::vector<LogicalProjectionItem> aggregation_items);

  [[nodiscard]] const std::vector<LogicalProjectionItem> &GroupingItems()
      const noexcept {
    return grouping_items_;
  }
  [[nodiscard]] const std::vector<LogicalProjectionItem> &AggregationItems()
      const noexcept {
    return aggregation_items_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<LogicalProjectionItem> grouping_items_;
  std::vector<LogicalProjectionItem> aggregation_items_;
};

class SortPlan final : public LogicalPlan {
 public:
  SortPlan(LogicalPlanPtr source, std::vector<LogicalSortItem> items);

  [[nodiscard]] const std::vector<LogicalSortItem> &Items() const noexcept {
    return items_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<LogicalSortItem> items_;
};

class SkipPlan final : public LogicalPlan {
 public:
  SkipPlan(LogicalPlanPtr source, const ast::Expression *skip);
  SkipPlan(LogicalPlanPtr source, const ast::Expression *skip,
           std::vector<LogicalPrecomputedExpression> precomputed_expressions);

  [[nodiscard]] const ast::Expression *Skip() const noexcept { return skip_; }
  [[nodiscard]] const std::vector<LogicalPrecomputedExpression> &
  PrecomputedExpressions() const noexcept {
    return precomputed_expressions_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *skip_ = nullptr;
  std::vector<LogicalPrecomputedExpression> precomputed_expressions_;
};

class LimitPlan final : public LogicalPlan {
 public:
  LimitPlan(LogicalPlanPtr source, const ast::Expression *limit);
  LimitPlan(LogicalPlanPtr source, const ast::Expression *limit,
            std::vector<LogicalPrecomputedExpression> precomputed_expressions);

  [[nodiscard]] const ast::Expression *Limit() const noexcept { return limit_; }
  [[nodiscard]] const std::vector<LogicalPrecomputedExpression> &
  PrecomputedExpressions() const noexcept {
    return precomputed_expressions_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *limit_ = nullptr;
  std::vector<LogicalPrecomputedExpression> precomputed_expressions_;
};

class ProduceResultsPlan final : public LogicalPlan {
 public:
  ProduceResultsPlan(LogicalPlanPtr source, std::vector<std::string> columns);

  [[nodiscard]] std::string Details() const override;
};

class CartesianProductPlan final : public LogicalPlan {
 public:
  CartesianProductPlan(LogicalPlanPtr left, LogicalPlanPtr right);
};

class NodeHashJoinPlan final : public LogicalPlan {
 public:
  NodeHashJoinPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                   std::vector<std::string> join_keys);

  [[nodiscard]] const std::vector<std::string> &JoinKeys() const noexcept {
    return join_keys_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<std::string> join_keys_;
};

class ValueHashJoinPlan final : public LogicalPlan {
 public:
  ValueHashJoinPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                    std::vector<const ast::Expression *> predicates);

  [[nodiscard]] const std::vector<const ast::Expression *> &Predicates()
      const noexcept {
    return predicates_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<const ast::Expression *> predicates_;
};

class PredicateJoinPlan final : public LogicalPlan {
 public:
  PredicateJoinPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                    std::vector<const ast::Expression *> predicates);

  [[nodiscard]] const std::vector<const ast::Expression *> &Predicates()
      const noexcept {
    return predicates_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<const ast::Expression *> predicates_;
};

class ApplyPlan final : public LogicalPlan {
 public:
  ApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right);
};

class SemiApplyPlan final : public LogicalPlan {
 public:
  SemiApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right);
};

class AntiSemiApplyPlan final : public LogicalPlan {
 public:
  AntiSemiApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right);
};

class LetSemiApplyPlan final : public LogicalPlan {
 public:
  LetSemiApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                   std::string value_variable);

  [[nodiscard]] const std::string &ValueVariable() const noexcept {
    return value_variable_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string value_variable_;
};

class RollUpApplyPlan final : public LogicalPlan {
 public:
  RollUpApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right,
                  std::string collection_variable, std::string value_variable);

  [[nodiscard]] const std::string &CollectionVariable() const noexcept {
    return collection_variable_;
  }
  [[nodiscard]] const std::string &ValueVariable() const noexcept {
    return value_variable_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string collection_variable_;
  std::string value_variable_;
};

class OptionalApplyPlan final : public LogicalPlan {
 public:
  OptionalApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right);
};

class AssertIsNodePlan final : public LogicalPlan {
 public:
  AssertIsNodePlan(LogicalPlanPtr source, std::vector<std::string> variables);

  [[nodiscard]] const std::vector<std::string> &Variables() const noexcept {
    return variables_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<std::string> variables_;
};

class WriteBarrierPlan final : public LogicalPlan {
 public:
  explicit WriteBarrierPlan(LogicalPlanPtr source);
};

class CreateNodePlan final : public LogicalPlan {
 public:
  CreateNodePlan(LogicalPlanPtr source, CreateNodePattern node);

  [[nodiscard]] const CreateNodePattern &Node() const noexcept { return node_; }
  [[nodiscard]] std::string Details() const override;

 private:
  CreateNodePattern node_;
};

class CreateRelationshipPlan final : public LogicalPlan {
 public:
  CreateRelationshipPlan(LogicalPlanPtr source,
                         CreateRelationshipPattern relationship);

  [[nodiscard]] const CreateRelationshipPattern &Relationship() const noexcept {
    return relationship_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  CreateRelationshipPattern relationship_;
};

class MergePlan final : public LogicalPlan {
 public:
  MergePlan(LogicalPlanPtr source, LogicalPlanPtr match_plan,
            MergePattern merge);

  [[nodiscard]] const MergePattern &Merge() const noexcept { return merge_; }
  [[nodiscard]] std::string Details() const override;

 private:
  MergePattern merge_;
};

class SetPropertyPlan final : public LogicalPlan {
 public:
  SetPropertyPlan(LogicalPlanPtr source, const ast::Expression *entity,
                  std::string property_key, const ast::Expression *value);

  [[nodiscard]] const ast::Expression *Entity() const noexcept {
    return entity_;
  }
  [[nodiscard]] const std::string &PropertyKey() const noexcept {
    return property_key_;
  }
  [[nodiscard]] const ast::Expression *Value() const noexcept { return value_; }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *entity_ = nullptr;
  std::string property_key_;
  const ast::Expression *value_ = nullptr;
};

class SetPropertiesPlan final : public LogicalPlan {
 public:
  SetPropertiesPlan(LogicalPlanPtr source, const ast::Expression *entity,
                    const ast::Expression *value, bool include_existing);

  [[nodiscard]] const ast::Expression *Entity() const noexcept {
    return entity_;
  }
  [[nodiscard]] const ast::Expression *Value() const noexcept { return value_; }
  [[nodiscard]] bool IncludeExisting() const noexcept {
    return include_existing_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *entity_ = nullptr;
  const ast::Expression *value_ = nullptr;
  bool include_existing_ = false;
};

class SetLabelsPlan final : public LogicalPlan {
 public:
  SetLabelsPlan(LogicalPlanPtr source, const ast::Expression *entity,
                std::vector<std::string> labels);

  [[nodiscard]] const ast::Expression *Entity() const noexcept {
    return entity_;
  }
  [[nodiscard]] const std::vector<std::string> &Labels() const noexcept {
    return labels_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *entity_ = nullptr;
  std::vector<std::string> labels_;
};

class RemovePropertyPlan final : public LogicalPlan {
 public:
  RemovePropertyPlan(LogicalPlanPtr source, const ast::Expression *entity,
                     std::string property_key);

  [[nodiscard]] const ast::Expression *Entity() const noexcept {
    return entity_;
  }
  [[nodiscard]] const std::string &PropertyKey() const noexcept {
    return property_key_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *entity_ = nullptr;
  std::string property_key_;
};

class RemoveLabelsPlan final : public LogicalPlan {
 public:
  RemoveLabelsPlan(LogicalPlanPtr source, const ast::Expression *entity,
                   std::vector<std::string> labels);

  [[nodiscard]] const ast::Expression *Entity() const noexcept {
    return entity_;
  }
  [[nodiscard]] const std::vector<std::string> &Labels() const noexcept {
    return labels_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *entity_ = nullptr;
  std::vector<std::string> labels_;
};

class DeletePlan final : public LogicalPlan {
 public:
  DeletePlan(LogicalPlanPtr source,
             std::vector<const ast::Expression *> expressions);

  [[nodiscard]] const std::vector<const ast::Expression *> &Expressions()
      const noexcept {
    return expressions_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<const ast::Expression *> expressions_;
};

class DetachDeletePlan final : public LogicalPlan {
 public:
  DetachDeletePlan(LogicalPlanPtr source,
                   std::vector<const ast::Expression *> expressions);

  [[nodiscard]] const std::vector<const ast::Expression *> &Expressions()
      const noexcept {
    return expressions_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<const ast::Expression *> expressions_;
};

class UnwindPlan final : public LogicalPlan {
 public:
  UnwindPlan(LogicalPlanPtr source, const ast::Expression *expression,
             std::string alias);

  [[nodiscard]] const ast::Expression *Expression() const noexcept {
    return expression_;
  }
  [[nodiscard]] const std::string &Alias() const noexcept { return alias_; }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *expression_ = nullptr;
  std::string alias_;
};

class ProcedureCallPlan final : public LogicalPlan {
 public:
  ProcedureCallPlan(LogicalPlanPtr source, std::string procedure_name,
                    std::vector<const ast::Expression *> arguments,
                    std::vector<ProcedureYieldItem> yield_items,
                    bool yield_star, bool read_only);

  [[nodiscard]] const std::string &ProcedureName() const noexcept {
    return procedure_name_;
  }
  [[nodiscard]] const std::vector<const ast::Expression *> &Arguments()
      const noexcept {
    return arguments_;
  }
  [[nodiscard]] const std::vector<ProcedureYieldItem> &YieldItems()
      const noexcept {
    return yield_items_;
  }
  [[nodiscard]] bool YieldStar() const noexcept { return yield_star_; }
  [[nodiscard]] bool ReadOnly() const noexcept { return read_only_; }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string procedure_name_;
  std::vector<const ast::Expression *> arguments_;
  std::vector<ProcedureYieldItem> yield_items_;
  bool yield_star_ = false;
  bool read_only_ = false;
};

class UnionPlan final : public LogicalPlan {
 public:
  UnionPlan(LogicalPlanPtr left, LogicalPlanPtr right,
            std::vector<LogicalUnionMapping> mappings, bool all);

  [[nodiscard]] const std::vector<LogicalUnionMapping> &Mappings()
      const noexcept {
    return mappings_;
  }
  [[nodiscard]] bool All() const noexcept { return all_; }
  [[nodiscard]] std::string Details() const override;

 private:
  std::vector<LogicalUnionMapping> mappings_;
  bool all_ = false;
};

}  // namespace ir
