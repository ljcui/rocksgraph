#include "ir/logical_plan.h"

#include <utility>

#include "common/exception.h"

namespace ir {

namespace {

std::unordered_set<std::string> UnionSymbols(
    std::unordered_set<std::string> left,
    const std::unordered_set<std::string> &right) {
  left.insert(right.begin(), right.end());
  return left;
}

}  // namespace

LogicalUnaryPlan::LogicalUnaryPlan(LogicalPlanNodeType node_type,
                                   std::unique_ptr<LogicalPlan> source)
    : LogicalPlan(node_type), source(std::move(source)) {
  CHECK(this->source != nullptr, common::InvalidArgumentError,
        "logical unary plan source is null");
}

LogicalBinaryPlan::LogicalBinaryPlan(LogicalPlanNodeType node_type,
                                     std::unique_ptr<LogicalPlan> left,
                                     std::unique_ptr<LogicalPlan> right)
    : LogicalPlan(node_type), left(std::move(left)), right(std::move(right)) {
  CHECK(this->left != nullptr, common::InvalidArgumentError,
        "logical binary plan left child is null");
  CHECK(this->right != nullptr, common::InvalidArgumentError,
        "logical binary plan right child is null");
}

Argument::Argument(std::unordered_set<std::string> argument_ids)
    : LogicalLeafPlan(LogicalPlanNodeType::kArgument),
      argument_ids(std::move(argument_ids)) {}

AllNodesScan::AllNodesScan(std::string id_name,
                           std::unordered_set<std::string> argument_ids)
    : LogicalLeafPlan(LogicalPlanNodeType::kAllNodesScan),
      id_name(std::move(id_name)),
      argument_ids(std::move(argument_ids)) {
  CHECK(!this->id_name.empty(), common::InvalidArgumentError,
        "all nodes scan id_name is empty");
}

std::unordered_set<std::string> AllNodesScan::AvailableSymbols() const {
  std::unordered_set<std::string> symbols = argument_ids;
  symbols.insert(id_name);
  return symbols;
}

NodeByLabelScan::NodeByLabelScan(std::string id_name, std::string label,
                                 std::unordered_set<std::string> argument_ids)
    : LogicalLeafPlan(LogicalPlanNodeType::kNodeByLabelScan),
      id_name(std::move(id_name)),
      label(std::move(label)),
      argument_ids(std::move(argument_ids)) {
  CHECK(!this->id_name.empty(), common::InvalidArgumentError,
        "node by label scan id_name is empty");
  CHECK(!this->label.empty(), common::InvalidArgumentError,
        "node by label scan label is empty");
}

std::unordered_set<std::string> NodeByLabelScan::AvailableSymbols() const {
  std::unordered_set<std::string> symbols = argument_ids;
  symbols.insert(id_name);
  return symbols;
}

Expand::Expand(std::unique_ptr<LogicalPlan> source, std::string from,
               std::string relationship, std::string to, Direction direction,
               std::unordered_set<std::string> types)
    : LogicalUnaryPlan(LogicalPlanNodeType::kExpand, std::move(source)),
      from(std::move(from)),
      relationship(std::move(relationship)),
      to(std::move(to)),
      direction(direction),
      types(std::move(types)) {
  CHECK(!this->from.empty(), common::InvalidArgumentError,
        "expand from is empty");
  CHECK(!this->relationship.empty(), common::InvalidArgumentError,
        "expand relationship is empty");
  CHECK(!this->to.empty(), common::InvalidArgumentError, "expand to is empty");
}

std::unordered_set<std::string> Expand::AvailableSymbols() const {
  auto symbols = source->AvailableSymbols();
  symbols.insert(relationship);
  symbols.insert(to);
  return symbols;
}

Selection::Selection(std::unique_ptr<LogicalPlan> source,
                     std::vector<const ast::Expression *> predicates)
    : LogicalUnaryPlan(LogicalPlanNodeType::kSelection, std::move(source)),
      predicates(std::move(predicates)) {
  CHECK(!this->predicates.empty(), common::InvalidArgumentError,
        "selection predicates are empty");
  for (const ast::Expression *predicate : this->predicates) {
    CHECK(predicate != nullptr, common::InvalidArgumentError,
          "selection predicate is null");
  }
}

Unwind::Unwind(std::unique_ptr<LogicalPlan> source,
               const ast::Expression *expression, std::string alias)
    : LogicalUnaryPlan(LogicalPlanNodeType::kUnwind, std::move(source)),
      expression(expression),
      alias(std::move(alias)) {
  CHECK(this->expression != nullptr, common::InvalidArgumentError,
        "unwind expression is null");
  CHECK(!this->alias.empty(), common::InvalidArgumentError,
        "unwind alias is empty");
}

std::unordered_set<std::string> Unwind::AvailableSymbols() const {
  std::unordered_set<std::string> symbols = source->AvailableSymbols();
  symbols.insert(alias);
  return symbols;
}

Project::Project(std::unique_ptr<LogicalPlan> source,
                 std::vector<ProjectItem> items, bool distinct)
    : LogicalUnaryPlan(LogicalPlanNodeType::kProject, std::move(source)),
      distinct(distinct),
      items(std::move(items)) {
  CHECK(!this->items.empty(), common::InvalidArgumentError,
        "project items are empty");
  for (const auto &item : this->items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "project item expression is null");
    CHECK(!item.alias.empty(), common::InvalidArgumentError,
          "project item alias is empty");
  }
}

std::unordered_set<std::string> Project::AvailableSymbols() const {
  std::unordered_set<std::string> symbols;
  for (const auto &item : items) {
    symbols.insert(item.alias);
  }
  return symbols;
}

Sort::Sort(std::unique_ptr<LogicalPlan> source, std::vector<OrderItem> items)
    : LogicalUnaryPlan(LogicalPlanNodeType::kSort, std::move(source)),
      items(std::move(items)) {
  CHECK(!this->items.empty(), common::InvalidArgumentError,
        "sort items are empty");
  for (const auto &item : this->items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "sort expression is null");
  }
}

Skip::Skip(std::unique_ptr<LogicalPlan> source, const ast::Expression *count)
    : LogicalUnaryPlan(LogicalPlanNodeType::kSkip, std::move(source)),
      count(count) {
  CHECK(this->count != nullptr, common::InvalidArgumentError,
        "skip count is null");
}

Limit::Limit(std::unique_ptr<LogicalPlan> source, const ast::Expression *count)
    : LogicalUnaryPlan(LogicalPlanNodeType::kLimit, std::move(source)),
      count(count) {
  CHECK(this->count != nullptr, common::InvalidArgumentError,
        "limit count is null");
}

ProduceResult::ProduceResult(std::unique_ptr<LogicalPlan> source,
                             std::vector<std::string> columns)
    : LogicalUnaryPlan(LogicalPlanNodeType::kProduceResult, std::move(source)),
      columns(std::move(columns)) {
  CHECK(!this->columns.empty(), common::InvalidArgumentError,
        "produce result columns are empty");
  for (const std::string &column : this->columns) {
    CHECK(!column.empty(), common::InvalidArgumentError,
          "produce result column is empty");
  }
}

CartesianProduct::CartesianProduct(std::unique_ptr<LogicalPlan> left,
                                   std::unique_ptr<LogicalPlan> right)
    : LogicalBinaryPlan(LogicalPlanNodeType::kCartesianProduct, std::move(left),
                        std::move(right)) {}

std::unordered_set<std::string> CartesianProduct::AvailableSymbols() const {
  return UnionSymbols(left->AvailableSymbols(), right->AvailableSymbols());
}

NodeHashJoin::NodeHashJoin(std::unique_ptr<LogicalPlan> left,
                           std::unique_ptr<LogicalPlan> right,
                           std::unordered_set<std::string> join_symbols)
    : LogicalBinaryPlan(LogicalPlanNodeType::kNodeHashJoin, std::move(left),
                        std::move(right)),
      join_symbols(std::move(join_symbols)) {
  CHECK(!this->join_symbols.empty(), common::InvalidArgumentError,
        "node hash join symbols are empty");
}

std::unordered_set<std::string> NodeHashJoin::AvailableSymbols() const {
  return UnionSymbols(left->AvailableSymbols(), right->AvailableSymbols());
}

Union::Union(std::unique_ptr<LogicalPlan> left,
             std::unique_ptr<LogicalPlan> right, bool all)
    : LogicalBinaryPlan(LogicalPlanNodeType::kUnion, std::move(left),
                        std::move(right)),
      all(all) {}

std::unordered_set<std::string> Union::AvailableSymbols() const {
  return left->AvailableSymbols();
}

std::vector<const LogicalPlan *> FlattenLogicalPlan(const LogicalPlan &root) {
  std::vector<const LogicalPlan *> plans;
  std::vector<const LogicalPlan *> stack = {&root};
  while (!stack.empty()) {
    const LogicalPlan *plan = stack.back();
    stack.pop_back();
    CHECK(plan != nullptr, common::InternalError,
          "null plan in logical plan stack");
    plans.push_back(plan);
    if (plan->Rhs() != nullptr) {
      stack.push_back(plan->Rhs());
    }
    if (plan->Lhs() != nullptr) {
      stack.push_back(plan->Lhs());
    }
  }
  return plans;
}

const LogicalPlan &LeftmostLeaf(const LogicalPlan &root) {
  const LogicalPlan *current = &root;
  while (current->Lhs() != nullptr) {
    current = current->Lhs();
  }
  return *current;
}

}  // namespace ir
