#include "ir/idp_planner.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/exception.h"

namespace ir {

namespace {

using RelationshipMask = std::uint64_t;

struct PlanExpr {
  enum class Kind { kSeedExpand, kExpand, kCartesianProduct, kNodeHashJoin };

  Kind kind = Kind::kSeedExpand;
  std::shared_ptr<const PlanExpr> lhs;
  std::shared_ptr<const PlanExpr> rhs;

  std::size_t relationship_index = 0;
  std::string from;
  std::string to;
  Expand::Direction direction = Expand::Direction::kBoth;
  bool seed_from_argument = false;
  std::unordered_set<std::string> join_symbols;

  std::unordered_set<std::string> solved_nodes;
  std::unordered_set<std::string> available_symbols;
  std::size_t cost = 0;
  std::string tie_breaker;
};

std::unordered_set<std::string> MergeSymbols(
    std::unordered_set<std::string> left,
    const std::unordered_set<std::string> &right) {
  left.insert(right.begin(), right.end());
  return left;
}

Expand::Direction ToExpandDirection(Direction direction) {
  switch (direction) {
    case Direction::kIncoming:
      return Expand::Direction::kIncoming;
    case Direction::kOutgoing:
      return Expand::Direction::kOutgoing;
    case Direction::kBoth:
      return Expand::Direction::kBoth;
  }
  THROW(common::InternalError, "unknown relationship direction");
}

Expand::Direction ReverseDirection(Expand::Direction direction) {
  switch (direction) {
    case Expand::Direction::kIncoming:
      return Expand::Direction::kOutgoing;
    case Expand::Direction::kOutgoing:
      return Expand::Direction::kIncoming;
    case Expand::Direction::kBoth:
      return Expand::Direction::kBoth;
  }
  THROW(common::InternalError, "unknown expand direction");
}

void ValidateQueryGraph(const QueryGraph &query_graph) {
  std::unordered_set<std::string> relationship_names;
  for (const std::string &node : query_graph.pattern_nodes) {
    CHECK(!node.empty(), common::InvalidArgumentError,
          "query graph contains empty node variable");
  }
  for (const auto &relationship : query_graph.pattern_relationships) {
    CHECK(!relationship.variable.empty(), common::InvalidArgumentError,
          "query graph relationship variable is empty");
    CHECK(!relationship.left_node.empty(), common::InvalidArgumentError,
          "query graph relationship left node is empty");
    CHECK(!relationship.right_node.empty(), common::InvalidArgumentError,
          "query graph relationship right node is empty");
    CHECK(query_graph.pattern_nodes.contains(relationship.left_node),
          common::InvalidArgumentError,
          "query graph relationship left node is missing from node set: " +
              relationship.left_node);
    CHECK(query_graph.pattern_nodes.contains(relationship.right_node),
          common::InvalidArgumentError,
          "query graph relationship right node is missing from node set: " +
              relationship.right_node);
    CHECK(!relationship.length.variable && relationship.length.fixed == 1,
          common::InvalidArgumentError,
          "IDP logical planning only supports fixed length relationships");
    CHECK(relationship_names.insert(relationship.variable).second,
          common::InvalidArgumentError,
          "duplicate relationship variable in query graph: " +
              relationship.variable);
  }
  for (const auto &predicate : query_graph.selections.predicates) {
    CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
          "query graph selection expression is null");
    for (const std::string &dependency : predicate.dependencies) {
      CHECK(!dependency.empty(), common::InvalidArgumentError,
            "query graph selection dependency is empty");
    }
  }
}

void ValidateArgumentSymbols(
    const std::unordered_set<std::string> &argument_symbols) {
  for (const std::string &symbol : argument_symbols) {
    CHECK(!symbol.empty(), common::InvalidArgumentError,
          "argument symbol is empty");
  }
}

std::shared_ptr<const PlanExpr> PickBetter(
    std::shared_ptr<const PlanExpr> current,
    const std::shared_ptr<const PlanExpr> &candidate) {
  if (!candidate) {
    return current;
  }
  if (!current) {
    return candidate;
  }
  if (candidate->cost < current->cost) {
    return candidate;
  }
  if (candidate->cost > current->cost) {
    return current;
  }
  if (candidate->tie_breaker < current->tie_breaker) {
    return candidate;
  }
  return current;
}

std::shared_ptr<const PlanExpr> MakeSeedPlan(
    const PatternRelationship &relationship, std::size_t relationship_index,
    bool from_left, const std::unordered_set<std::string> &argument_symbols,
    const IDPPlannerConfig &config) {
  constexpr std::size_t kScanCost = 1;

  auto plan = std::make_shared<PlanExpr>();
  plan->kind = PlanExpr::Kind::kSeedExpand;
  plan->relationship_index = relationship_index;

  const Expand::Direction left_to_right =
      ToExpandDirection(relationship.direction);
  if (from_left) {
    plan->from = relationship.left_node;
    plan->to = relationship.right_node;
    plan->direction = left_to_right;
  } else {
    plan->from = relationship.right_node;
    plan->to = relationship.left_node;
    plan->direction = ReverseDirection(left_to_right);
  }

  plan->seed_from_argument = argument_symbols.contains(plan->from);
  plan->solved_nodes.insert(relationship.left_node);
  plan->solved_nodes.insert(relationship.right_node);
  plan->solved_nodes.insert(argument_symbols.begin(), argument_symbols.end());
  plan->available_symbols.insert(argument_symbols.begin(),
                                 argument_symbols.end());
  plan->available_symbols.insert(plan->from);
  plan->available_symbols.insert(plan->to);
  plan->available_symbols.insert(relationship.variable);
  plan->cost = config.expand_cost + (plan->seed_from_argument ? 0 : kScanCost);
  plan->tie_breaker = "seed(" + relationship.variable + ":" + plan->from +
                      "->" + plan->to + ")";
  return plan;
}

std::shared_ptr<const PlanExpr> MakeExpandPlan(
    const std::shared_ptr<const PlanExpr> &base,
    const PatternRelationship &relationship, std::size_t relationship_index,
    bool from_left, const IDPPlannerConfig &config) {
  CHECK(base != nullptr, common::InternalError, "base IDP plan is null");

  auto plan = std::make_shared<PlanExpr>();
  plan->kind = PlanExpr::Kind::kExpand;
  plan->lhs = base;
  plan->relationship_index = relationship_index;

  const Expand::Direction left_to_right =
      ToExpandDirection(relationship.direction);
  if (from_left) {
    plan->from = relationship.left_node;
    plan->to = relationship.right_node;
    plan->direction = left_to_right;
  } else {
    plan->from = relationship.right_node;
    plan->to = relationship.left_node;
    plan->direction = ReverseDirection(left_to_right);
  }

  if (!base->solved_nodes.contains(plan->from)) {
    return nullptr;
  }

  const bool expand_into = base->solved_nodes.contains(plan->to);

  plan->solved_nodes = base->solved_nodes;
  plan->solved_nodes.insert(relationship.left_node);
  plan->solved_nodes.insert(relationship.right_node);
  plan->available_symbols = base->available_symbols;
  plan->available_symbols.insert(relationship.variable);
  plan->available_symbols.insert(plan->to);
  plan->cost =
      base->cost + (expand_into ? config.expand_into_cost : config.expand_cost);
  plan->tie_breaker = base->tie_breaker + "|expand(" + relationship.variable +
                      ":" + plan->from + "->" + plan->to + ")";
  return plan;
}

std::shared_ptr<const PlanExpr> MakeCartesianPlan(
    const std::shared_ptr<const PlanExpr> &left,
    const std::shared_ptr<const PlanExpr> &right,
    const IDPPlannerConfig &config) {
  CHECK(left != nullptr, common::InternalError, "left IDP plan is null");
  CHECK(right != nullptr, common::InternalError, "right IDP plan is null");

  auto plan = std::make_shared<PlanExpr>();
  plan->kind = PlanExpr::Kind::kCartesianProduct;
  plan->lhs = left;
  plan->rhs = right;
  plan->solved_nodes = MergeSymbols(left->solved_nodes, right->solved_nodes);
  plan->available_symbols =
      MergeSymbols(left->available_symbols, right->available_symbols);
  plan->cost = left->cost + right->cost + config.cartesian_product_cost;
  plan->tie_breaker =
      "cp(" + left->tie_breaker + "," + right->tie_breaker + ")";
  return plan;
}

std::shared_ptr<const PlanExpr> MakeNodeHashJoinPlan(
    const std::shared_ptr<const PlanExpr> &left,
    const std::shared_ptr<const PlanExpr> &right,
    std::unordered_set<std::string> join_symbols,
    const IDPPlannerConfig &config) {
  CHECK(left != nullptr, common::InternalError, "left IDP plan is null");
  CHECK(right != nullptr, common::InternalError, "right IDP plan is null");
  CHECK(!join_symbols.empty(), common::InvalidArgumentError,
        "node hash join symbols are empty");

  auto plan = std::make_shared<PlanExpr>();
  plan->kind = PlanExpr::Kind::kNodeHashJoin;
  plan->lhs = left;
  plan->rhs = right;
  plan->join_symbols = std::move(join_symbols);
  plan->solved_nodes = MergeSymbols(left->solved_nodes, right->solved_nodes);
  plan->available_symbols =
      MergeSymbols(left->available_symbols, right->available_symbols);
  // Join should be competitive with chained expands when both sides are
  // already cheap seeds (for example, with argument symbols in later parts).
  plan->cost = left->cost + right->cost;
  plan->tie_breaker =
      "join(" + left->tie_breaker + "," + right->tie_breaker + ")";
  return plan;
}

std::unique_ptr<LogicalPlan> BuildLogicalPlanFromExpr(
    const std::shared_ptr<const PlanExpr> &expr,
    const std::vector<PatternRelationship> &relationships,
    const std::unordered_set<std::string> &argument_symbols) {
  CHECK(expr != nullptr, common::InternalError, "IDP plan expression is null");

  switch (expr->kind) {
    case PlanExpr::Kind::kSeedExpand: {
      CHECK(expr->relationship_index < relationships.size(),
            common::InternalError, "seed relationship index out of range");
      const auto &relationship = relationships[expr->relationship_index];
      std::unique_ptr<LogicalPlan> source;
      if (expr->seed_from_argument) {
        source = std::make_unique<Argument>(argument_symbols);
      } else {
        source = std::make_unique<AllNodesScan>(expr->from, argument_symbols);
      }
      return std::make_unique<Expand>(
          std::move(source), expr->from, relationship.variable, expr->to,
          expr->direction,
          std::unordered_set<std::string>(relationship.types.begin(),
                                          relationship.types.end()));
    }
    case PlanExpr::Kind::kExpand: {
      CHECK(expr->relationship_index < relationships.size(),
            common::InternalError, "expand relationship index out of range");
      CHECK(expr->lhs != nullptr, common::InternalError,
            "expand source plan expression is null");
      const auto &relationship = relationships[expr->relationship_index];
      return std::make_unique<Expand>(
          BuildLogicalPlanFromExpr(expr->lhs, relationships, argument_symbols),
          expr->from, relationship.variable, expr->to, expr->direction,
          std::unordered_set<std::string>(relationship.types.begin(),
                                          relationship.types.end()));
    }
    case PlanExpr::Kind::kCartesianProduct: {
      CHECK(expr->lhs != nullptr, common::InternalError,
            "cartesian product left plan expression is null");
      CHECK(expr->rhs != nullptr, common::InternalError,
            "cartesian product right plan expression is null");
      return std::make_unique<CartesianProduct>(
          BuildLogicalPlanFromExpr(expr->lhs, relationships, argument_symbols),
          BuildLogicalPlanFromExpr(expr->rhs, relationships, argument_symbols));
    }
    case PlanExpr::Kind::kNodeHashJoin: {
      CHECK(expr->lhs != nullptr, common::InternalError,
            "node hash join left plan expression is null");
      CHECK(expr->rhs != nullptr, common::InternalError,
            "node hash join right plan expression is null");
      CHECK(!expr->join_symbols.empty(), common::InternalError,
            "node hash join symbols are empty");
      return std::make_unique<NodeHashJoin>(
          BuildLogicalPlanFromExpr(expr->lhs, relationships, argument_symbols),
          BuildLogicalPlanFromExpr(expr->rhs, relationships, argument_symbols),
          expr->join_symbols);
    }
  }
  THROW(common::InternalError, "unknown plan expression kind");
}

void AppendUncoveredNodeScans(
    const QueryGraph &query_graph,
    const std::unordered_set<std::string> &solved_nodes,
    std::unique_ptr<LogicalPlan> *plan) {
  CHECK(plan != nullptr, common::InternalError, "output plan is null");

  std::vector<std::string> uncovered_nodes;
  uncovered_nodes.reserve(query_graph.pattern_nodes.size());
  for (const std::string &node : query_graph.pattern_nodes) {
    if (!solved_nodes.contains(node)) {
      uncovered_nodes.push_back(node);
    }
  }
  std::sort(uncovered_nodes.begin(), uncovered_nodes.end());

  for (const std::string &node : uncovered_nodes) {
    auto node_scan = std::make_unique<AllNodesScan>(node);
    if (*plan == nullptr) {
      *plan = std::move(node_scan);
      continue;
    }
    *plan = std::make_unique<CartesianProduct>(std::move(*plan),
                                               std::move(node_scan));
  }
}

std::vector<const ast::Expression *> ExpressionsFromSelections(
    const std::vector<Predicate> &predicates) {
  std::vector<const ast::Expression *> expressions;
  expressions.reserve(predicates.size());
  for (const auto &predicate : predicates) {
    CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
          "query graph selection expression is null");
    expressions.push_back(predicate.expression);
  }
  return expressions;
}

bool DependenciesCoveredBy(const Predicate &predicate,
                           const std::unordered_set<std::string> &symbols) {
  for (const std::string &dependency : predicate.dependencies) {
    if (!symbols.contains(dependency)) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<LogicalPlan> AttachSelection(
    std::unique_ptr<LogicalPlan> plan,
    const std::vector<Predicate> &predicates) {
  CHECK(plan != nullptr, common::InternalError,
        "logical plan for selection attach is null");
  if (predicates.empty()) {
    return plan;
  }
  return std::make_unique<Selection>(std::move(plan),
                                     ExpressionsFromSelections(predicates));
}

std::unique_ptr<LogicalPlan> PushDownWherePredicates(
    std::unique_ptr<LogicalPlan> plan,
    const std::vector<Predicate> &predicates) {
  CHECK(plan != nullptr, common::InternalError,
        "logical plan for predicate pushdown is null");
  if (predicates.empty()) {
    return plan;
  }

  switch (plan->node_type) {
    case LogicalPlanNodeType::kExpand: {
      auto *typed = static_cast<Expand *>(plan.get());
      const auto child_symbols = typed->source->AvailableSymbols();
      std::vector<Predicate> child_predicates;
      std::vector<Predicate> current_predicates;
      child_predicates.reserve(predicates.size());
      current_predicates.reserve(predicates.size());
      for (const auto &predicate : predicates) {
        if (DependenciesCoveredBy(predicate, child_symbols)) {
          child_predicates.push_back(predicate);
        } else {
          current_predicates.push_back(predicate);
        }
      }
      auto pushed_child =
          PushDownWherePredicates(std::move(typed->source), child_predicates);
      auto rebuilt = std::make_unique<Expand>(
          std::move(pushed_child), typed->from, typed->relationship, typed->to,
          typed->direction, typed->types);
      return AttachSelection(std::move(rebuilt), current_predicates);
    }
    case LogicalPlanNodeType::kCartesianProduct: {
      auto *typed = static_cast<CartesianProduct *>(plan.get());
      const auto left_symbols = typed->left->AvailableSymbols();
      const auto right_symbols = typed->right->AvailableSymbols();
      std::vector<Predicate> left_predicates;
      std::vector<Predicate> right_predicates;
      std::vector<Predicate> current_predicates;
      left_predicates.reserve(predicates.size());
      right_predicates.reserve(predicates.size());
      current_predicates.reserve(predicates.size());
      for (const auto &predicate : predicates) {
        if (DependenciesCoveredBy(predicate, left_symbols)) {
          left_predicates.push_back(predicate);
        } else if (DependenciesCoveredBy(predicate, right_symbols)) {
          right_predicates.push_back(predicate);
        } else {
          current_predicates.push_back(predicate);
        }
      }
      auto rebuilt = std::make_unique<CartesianProduct>(
          PushDownWherePredicates(std::move(typed->left), left_predicates),
          PushDownWherePredicates(std::move(typed->right), right_predicates));
      return AttachSelection(std::move(rebuilt), current_predicates);
    }
    case LogicalPlanNodeType::kNodeHashJoin: {
      auto *typed = static_cast<NodeHashJoin *>(plan.get());
      const auto left_symbols = typed->left->AvailableSymbols();
      const auto right_symbols = typed->right->AvailableSymbols();
      std::vector<Predicate> left_predicates;
      std::vector<Predicate> right_predicates;
      std::vector<Predicate> current_predicates;
      left_predicates.reserve(predicates.size());
      right_predicates.reserve(predicates.size());
      current_predicates.reserve(predicates.size());
      for (const auto &predicate : predicates) {
        if (DependenciesCoveredBy(predicate, left_symbols)) {
          left_predicates.push_back(predicate);
        } else if (DependenciesCoveredBy(predicate, right_symbols)) {
          right_predicates.push_back(predicate);
        } else {
          current_predicates.push_back(predicate);
        }
      }
      auto rebuilt = std::make_unique<NodeHashJoin>(
          PushDownWherePredicates(std::move(typed->left), left_predicates),
          PushDownWherePredicates(std::move(typed->right), right_predicates),
          typed->join_symbols);
      return AttachSelection(std::move(rebuilt), current_predicates);
    }
    case LogicalPlanNodeType::kArgument:
    case LogicalPlanNodeType::kAllNodesScan:
    case LogicalPlanNodeType::kNodeByLabelScan:
      return AttachSelection(std::move(plan), predicates);
    default:
      return AttachSelection(std::move(plan), predicates);
  }
}

std::unique_ptr<LogicalPlan> ApplyWherePredicates(
    const QueryGraph &query_graph, std::unique_ptr<LogicalPlan> plan) {
  CHECK(plan != nullptr, common::InternalError, "output plan is null");
  if (query_graph.selections.empty()) {
    return plan;
  }

  const auto available_symbols = plan->AvailableSymbols();
  for (const auto &predicate : query_graph.selections.predicates) {
    CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
          "query graph selection expression is null");
    for (const std::string &dependency : predicate.dependencies) {
      CHECK(available_symbols.contains(dependency),
            common::InvalidArgumentError,
            "selection dependency is not available in planned symbols: " +
                dependency);
    }
  }
  return PushDownWherePredicates(std::move(plan),
                                 query_graph.selections.predicates);
}

}  // namespace

std::unique_ptr<LogicalPlan> BuildIDPLogicalPlan(
    const QueryGraph &query_graph, const IDPPlannerConfig &config) {
  return BuildIDPLogicalPlan(query_graph, {}, config);
}

std::unique_ptr<LogicalPlan> BuildIDPLogicalPlan(
    const QueryGraph &query_graph,
    const std::unordered_set<std::string> &argument_symbols,
    const IDPPlannerConfig &config) {
  CHECK(config.max_relationships > 0, common::InvalidArgumentError,
        "IDP max_relationships must be positive");
  CHECK(config.expand_cost > 0, common::InvalidArgumentError,
        "IDP expand_cost must be positive");
  CHECK(config.expand_into_cost > 0, common::InvalidArgumentError,
        "IDP expand_into_cost must be positive");
  CHECK(config.cartesian_product_cost > 0, common::InvalidArgumentError,
        "IDP cartesian_product_cost must be positive");

  ValidateQueryGraph(query_graph);
  ValidateArgumentSymbols(argument_symbols);

  std::shared_ptr<const PlanExpr> full_plan_expr;
  std::unordered_set<std::string> solved_nodes = argument_symbols;

  const std::size_t relationship_count =
      query_graph.pattern_relationships.size();
  if (relationship_count > 0) {
    CHECK(relationship_count <= config.max_relationships,
          common::InvalidArgumentError,
          "query graph relationship count exceeds IDP max_relationships");
    CHECK(
        relationship_count < static_cast<std::size_t>(
                                 std::numeric_limits<RelationshipMask>::digits),
        common::InvalidArgumentError,
        "query graph relationship count exceeds IDP relationship mask limit");

    const RelationshipMask table_size = RelationshipMask{1}
                                        << relationship_count;
    std::vector<std::shared_ptr<const PlanExpr>> table(
        static_cast<std::size_t>(table_size));

    for (std::size_t i = 0; i < relationship_count; ++i) {
      const RelationshipMask mask = RelationshipMask{1} << i;
      const auto &relationship = query_graph.pattern_relationships[i];
      table[static_cast<std::size_t>(mask)] = PickBetter(
          table[static_cast<std::size_t>(mask)],
          MakeSeedPlan(relationship, i, true, argument_symbols, config));
      if (relationship.left_node != relationship.right_node) {
        table[static_cast<std::size_t>(mask)] = PickBetter(
            table[static_cast<std::size_t>(mask)],
            MakeSeedPlan(relationship, i, false, argument_symbols, config));
      }
    }

    for (RelationshipMask mask = 1; mask < table_size; ++mask) {
      if (std::popcount(mask) <= 1) {
        continue;
      }
      std::shared_ptr<const PlanExpr> best_for_mask;

      for (std::size_t i = 0; i < relationship_count; ++i) {
        const RelationshipMask bit = RelationshipMask{1} << i;
        if ((mask & bit) == 0) {
          continue;
        }
        const RelationshipMask previous_mask = mask & ~bit;
        const auto &previous = table[static_cast<std::size_t>(previous_mask)];
        if (!previous) {
          continue;
        }

        const auto &relationship = query_graph.pattern_relationships[i];
        best_for_mask =
            PickBetter(best_for_mask,
                       MakeExpandPlan(previous, relationship, i, true, config));
        if (relationship.left_node != relationship.right_node) {
          best_for_mask = PickBetter(
              best_for_mask,
              MakeExpandPlan(previous, relationship, i, false, config));
        }
      }

      for (RelationshipMask left_mask = (mask - 1) & mask; left_mask != 0;
           left_mask = (left_mask - 1) & mask) {
        const RelationshipMask right_mask = mask ^ left_mask;
        if (left_mask > right_mask) {
          continue;
        }
        const auto &left_plan = table[static_cast<std::size_t>(left_mask)];
        const auto &right_plan = table[static_cast<std::size_t>(right_mask)];
        if (!left_plan || !right_plan) {
          continue;
        }
        std::unordered_set<std::string> join_symbols;
        for (const std::string &symbol : left_plan->solved_nodes) {
          if (right_plan->solved_nodes.contains(symbol)) {
            join_symbols.insert(symbol);
          }
        }
        if (!join_symbols.empty()) {
          best_for_mask =
              PickBetter(best_for_mask,
                         MakeNodeHashJoinPlan(left_plan, right_plan,
                                              std::move(join_symbols), config));
          continue;
        }
        best_for_mask = PickBetter(
            best_for_mask, MakeCartesianPlan(left_plan, right_plan, config));
      }

      table[static_cast<std::size_t>(mask)] = best_for_mask;
    }

    const RelationshipMask full_mask = table_size - 1;
    full_plan_expr = table[static_cast<std::size_t>(full_mask)];
    CHECK(full_plan_expr != nullptr, common::InvalidArgumentError,
          "failed to build complete IDP plan for query graph relationships");
    solved_nodes = full_plan_expr->solved_nodes;
  }

  std::unique_ptr<LogicalPlan> plan;
  if (full_plan_expr) {
    plan = BuildLogicalPlanFromExpr(
        full_plan_expr, query_graph.pattern_relationships, argument_symbols);
  } else if (!argument_symbols.empty()) {
    plan = std::make_unique<Argument>(argument_symbols);
  }

  AppendUncoveredNodeScans(query_graph, solved_nodes, &plan);

  if (!plan) {
    plan = std::make_unique<Argument>();
  }

  return ApplyWherePredicates(query_graph, std::move(plan));
}

}  // namespace ir
