#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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
  kUnwind,
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

struct LogicalProjectionItem {
  const ast::Expression *expression = nullptr;
  std::string alias;
};

struct LogicalSortItem {
  const ast::Expression *expression = nullptr;
  LogicalOrderDirection direction = LogicalOrderDirection::kAscending;
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
  [[nodiscard]] const std::vector<LogicalPlanPtr> &Children() const noexcept {
    return children_;
  }
  [[nodiscard]] std::size_t ChildCount() const noexcept {
    return children_.size();
  }
  [[nodiscard]] const LogicalPlan &Child(std::size_t index) const;
  [[nodiscard]] LogicalPlan &Child(std::size_t index);

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
                    const ast::Expression *value_expression);

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
  [[nodiscard]] std::string Details() const override;

 private:
  std::string variable_;
  std::vector<std::string> labels_;
  std::string property_key_;
  const ast::Expression *value_expression_ = nullptr;
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
                            const ast::Expression *value_expression);

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
  [[nodiscard]] std::string Details() const override;

 private:
  std::string from_node_;
  std::string relationship_;
  std::string to_node_;
  ExpandDirection direction_ = ExpandDirection::kBoth;
  std::vector<std::string> types_;
  std::string property_key_;
  const ast::Expression *value_expression_ = nullptr;
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
  PathBuildPlan(LogicalPlanPtr source, std::string path_variable);

  [[nodiscard]] const std::string &PathVariable() const noexcept {
    return path_variable_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string path_variable_;
};

class FilterPlan final : public LogicalPlan {
 public:
  FilterPlan(LogicalPlanPtr source, const ast::Expression *predicate);

  [[nodiscard]] const ast::Expression *Predicate() const noexcept {
    return predicate_;
  }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *predicate_ = nullptr;
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

  [[nodiscard]] const ast::Expression *Skip() const noexcept { return skip_; }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *skip_ = nullptr;
};

class LimitPlan final : public LogicalPlan {
 public:
  LimitPlan(LogicalPlanPtr source, const ast::Expression *limit);

  [[nodiscard]] const ast::Expression *Limit() const noexcept { return limit_; }
  [[nodiscard]] std::string Details() const override;

 private:
  const ast::Expression *limit_ = nullptr;
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
