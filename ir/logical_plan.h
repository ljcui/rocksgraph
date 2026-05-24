#pragma once

#include <cstddef>
#include <memory>
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
  kExpand,
  kExpandInto,
  kFilter,
  kProjection,
  kDistinct,
  kAggregation,
  kSort,
  kSkip,
  kLimit,
  kProduceResults,
  kCartesianProduct,
  kApply,
  kSemiApply,
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

  [[nodiscard]] const std::string &Variable() const noexcept {
    return variable_;
  }
  [[nodiscard]] const std::string &Label() const noexcept { return label_; }
  [[nodiscard]] std::string Details() const override;

 private:
  std::string variable_;
  std::string label_;
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

class ApplyPlan final : public LogicalPlan {
 public:
  ApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right);
};

class SemiApplyPlan final : public LogicalPlan {
 public:
  SemiApplyPlan(LogicalPlanPtr left, LogicalPlanPtr right);
};

}  // namespace ir
