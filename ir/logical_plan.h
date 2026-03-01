#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ast/ast_node.h"

namespace ir {

#define LOGICAL_PLAN_NODE_TYPE_LIST(X)     \
  X(kUnknown, "Unknown")                   \
  X(kArgument, "Argument")                 \
  X(kAllNodesScan, "AllNodesScan")         \
  X(kNodeByLabelScan, "NodeByLabelScan")   \
  X(kExpand, "Expand")                     \
  X(kSelection, "Selection")               \
  X(kProject, "Project")                   \
  X(kSort, "Sort")                         \
  X(kSkip, "Skip")                         \
  X(kLimit, "Limit")                       \
  X(kProduceResult, "ProduceResult")       \
  X(kCartesianProduct, "CartesianProduct") \
  X(kNodeHashJoin, "NodeHashJoin")         \
  X(kUnion, "Union")

enum class LogicalPlanNodeType {
#define LOGICAL_PLAN_NODE_TYPE_ENUM(name, text) name,
  LOGICAL_PLAN_NODE_TYPE_LIST(LOGICAL_PLAN_NODE_TYPE_ENUM)
#undef LOGICAL_PLAN_NODE_TYPE_ENUM
};

inline constexpr auto kLogicalPlanNodeTypeNames = std::array{
#define LOGICAL_PLAN_NODE_TYPE_NAME(name, text) std::string_view{text},
    LOGICAL_PLAN_NODE_TYPE_LIST(LOGICAL_PLAN_NODE_TYPE_NAME)
#undef LOGICAL_PLAN_NODE_TYPE_NAME
};

static_assert(kLogicalPlanNodeTypeNames.size() ==
              static_cast<std::size_t>(LogicalPlanNodeType::kUnion) + 1);

inline constexpr std::string_view ToString(LogicalPlanNodeType type) {
  const auto index = static_cast<std::size_t>(type);
  if (index >= kLogicalPlanNodeTypeNames.size()) {
    return "Unknown";
  }
  return kLogicalPlanNodeTypeNames[index];
}

#undef LOGICAL_PLAN_NODE_TYPE_LIST

inline std::ostream &operator<<(std::ostream &out, LogicalPlanNodeType type) {
  out << ToString(type);
  return out;
}

class LogicalPlan {
 public:
  explicit LogicalPlan(LogicalPlanNodeType node_type) : node_type(node_type) {}
  virtual ~LogicalPlan() = default;

  LogicalPlanNodeType node_type = LogicalPlanNodeType::kUnknown;

  [[nodiscard]] virtual const LogicalPlan *Lhs() const = 0;
  [[nodiscard]] virtual const LogicalPlan *Rhs() const = 0;
  [[nodiscard]] virtual std::unordered_set<std::string> AvailableSymbols()
      const = 0;

  [[nodiscard]] bool IsLeaf() const {
    return Lhs() == nullptr && Rhs() == nullptr;
  }
};

class LogicalLeafPlan : public LogicalPlan {
 public:
  explicit LogicalLeafPlan(LogicalPlanNodeType node_type)
      : LogicalPlan(node_type) {}
  [[nodiscard]] const LogicalPlan *Lhs() const final { return nullptr; }
  [[nodiscard]] const LogicalPlan *Rhs() const final { return nullptr; }
};

class LogicalUnaryPlan : public LogicalPlan {
 public:
  LogicalUnaryPlan(LogicalPlanNodeType node_type,
                   std::unique_ptr<LogicalPlan> source);

  [[nodiscard]] const LogicalPlan *Lhs() const final { return source.get(); }
  [[nodiscard]] const LogicalPlan *Rhs() const final { return nullptr; }

  std::unique_ptr<LogicalPlan> source;
};

class LogicalBinaryPlan : public LogicalPlan {
 public:
  LogicalBinaryPlan(LogicalPlanNodeType node_type,
                    std::unique_ptr<LogicalPlan> left,
                    std::unique_ptr<LogicalPlan> right);

  [[nodiscard]] const LogicalPlan *Lhs() const final { return left.get(); }
  [[nodiscard]] const LogicalPlan *Rhs() const final { return right.get(); }

  std::unique_ptr<LogicalPlan> left;
  std::unique_ptr<LogicalPlan> right;
};

struct Argument final : public LogicalLeafPlan {
  explicit Argument(std::unordered_set<std::string> argument_ids = {});

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final {
    return argument_ids;
  }

  std::unordered_set<std::string> argument_ids;
};

struct AllNodesScan final : public LogicalLeafPlan {
  AllNodesScan(std::string id_name,
               std::unordered_set<std::string> argument_ids = {});

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final;

  std::string id_name;
  std::unordered_set<std::string> argument_ids;
};

struct NodeByLabelScan final : public LogicalLeafPlan {
  NodeByLabelScan(std::string id_name, std::string label,
                  std::unordered_set<std::string> argument_ids = {});

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final;

  std::string id_name;
  std::string label;
  std::unordered_set<std::string> argument_ids;
};

struct Expand final : public LogicalUnaryPlan {
  enum class Direction { kIncoming, kOutgoing, kBoth };

  Expand(std::unique_ptr<LogicalPlan> source, std::string from,
         std::string relationship, std::string to, Direction direction,
         std::unordered_set<std::string> types = {});

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final;

  std::string from;
  std::string relationship;
  std::string to;
  Direction direction = Direction::kBoth;
  std::unordered_set<std::string> types;
};

struct Selection final : public LogicalUnaryPlan {
  Selection(std::unique_ptr<LogicalPlan> source,
            std::vector<const ast::Expression *> predicates);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final {
    return source->AvailableSymbols();
  }

  std::vector<const ast::Expression *> predicates;
};

struct ProjectItem {
  const ast::Expression *expression = nullptr;
  std::string alias;
};

struct Project final : public LogicalUnaryPlan {
  Project(std::unique_ptr<LogicalPlan> source, std::vector<ProjectItem> items,
          bool distinct = false);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final;

  bool distinct = false;
  std::vector<ProjectItem> items;
};

struct OrderItem {
  const ast::Expression *expression = nullptr;
  bool ascending = true;
};

struct Sort final : public LogicalUnaryPlan {
  Sort(std::unique_ptr<LogicalPlan> source, std::vector<OrderItem> items);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final {
    return source->AvailableSymbols();
  }

  std::vector<OrderItem> items;
};

struct Skip final : public LogicalUnaryPlan {
  Skip(std::unique_ptr<LogicalPlan> source, const ast::Expression *count);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final {
    return source->AvailableSymbols();
  }

  const ast::Expression *count = nullptr;
};

struct Limit final : public LogicalUnaryPlan {
  Limit(std::unique_ptr<LogicalPlan> source, const ast::Expression *count);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final {
    return source->AvailableSymbols();
  }

  const ast::Expression *count = nullptr;
};

struct ProduceResult final : public LogicalUnaryPlan {
  ProduceResult(std::unique_ptr<LogicalPlan> source,
                std::vector<std::string> columns);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final {
    return source->AvailableSymbols();
  }

  std::vector<std::string> columns;
};

struct CartesianProduct final : public LogicalBinaryPlan {
  CartesianProduct(std::unique_ptr<LogicalPlan> left,
                   std::unique_ptr<LogicalPlan> right);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final;
};

struct NodeHashJoin final : public LogicalBinaryPlan {
  NodeHashJoin(std::unique_ptr<LogicalPlan> left,
               std::unique_ptr<LogicalPlan> right,
               std::unordered_set<std::string> join_symbols);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final;

  std::unordered_set<std::string> join_symbols;
};

struct Union final : public LogicalBinaryPlan {
  Union(std::unique_ptr<LogicalPlan> left, std::unique_ptr<LogicalPlan> right,
        bool all);

  [[nodiscard]] std::unordered_set<std::string> AvailableSymbols() const final;

  bool all = false;
};

std::vector<const LogicalPlan *> FlattenLogicalPlan(const LogicalPlan &root);
const LogicalPlan &LeftmostLeaf(const LogicalPlan &root);

}  // namespace ir
