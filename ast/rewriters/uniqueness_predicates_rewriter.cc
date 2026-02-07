#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "uniqueness_predicates_rewriter.h"

namespace ast {
namespace {

struct SingleRelationship {
  std::string name;
  std::vector<std::string> types;
};

struct RelationshipConnection {
  bool is_group = false;
  bool can_be_empty = false;
  std::vector<SingleRelationship> inner_relationships;
};

bool HasTypeOverlap(const SingleRelationship &lhs,
                    const SingleRelationship &rhs) {
  if (lhs.types.empty() || rhs.types.empty()) {
    return true;
  }
  std::unordered_set<std::string> lhs_types(lhs.types.begin(), lhs.types.end());
  return std::any_of(rhs.types.begin(), rhs.types.end(),
                     [&lhs_types](const std::string &type) {
                       return lhs_types.find(type) != lhs_types.end();
                     });
}

bool IsAlwaysDifferent(const SingleRelationship &lhs,
                       const SingleRelationship &rhs) {
  return !HasTypeOverlap(lhs, rhs);
}

std::unique_ptr<Expression> MakeVariable(const std::string &name) {
  auto node = std::make_unique<Variable>();
  node->name = name;
  return node;
}

std::unique_ptr<Expression> MakeBooleanLiteral(bool value) {
  auto node = std::make_unique<BooleanLiteral>();
  node->value = value;
  return node;
}

std::unique_ptr<Expression> MakeAnd(std::unique_ptr<Expression> left,
                                    std::unique_ptr<Expression> right) {
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  auto node = std::make_unique<AndExpression>();
  node->left = std::move(left);
  node->right = std::move(right);
  return node;
}

std::unique_ptr<Expression> MakeNot(std::unique_ptr<Expression> operand) {
  auto node = std::make_unique<NotExpression>();
  node->operand = std::move(operand);
  return node;
}

std::unique_ptr<Expression> MakeNotEquals(const std::string &left,
                                          const std::string &right) {
  auto eq = std::make_unique<ComparisonExpression>();
  eq->left = MakeVariable(left);
  eq->op = "=";
  eq->right = MakeVariable(right);
  return MakeNot(std::move(eq));
}

std::unique_ptr<Expression> MakeIn(std::unique_ptr<Expression> element,
                                   std::unique_ptr<Expression> list) {
  auto node = std::make_unique<ListPredicateExpression>();
  node->element = std::move(element);
  node->list = std::move(list);
  return node;
}

std::unique_ptr<Expression> MakeRelationshipListExpression(
    const std::vector<std::string> &relationship_names) {
  if (relationship_names.empty()) {
    return nullptr;
  }
  std::unique_ptr<Expression> list = MakeVariable(relationship_names.front());
  for (size_t i = 1; i < relationship_names.size(); ++i) {
    auto add = std::make_unique<AddExpression>();
    add->left = std::move(list);
    add->right = MakeVariable(relationship_names[i]);
    list = std::move(add);
  }
  return list;
}

std::unique_ptr<Expression> CombinePredicates(
    std::vector<std::unique_ptr<Expression>> &predicates) {
  std::unique_ptr<Expression> combined;
  for (auto &predicate : predicates) {
    combined = MakeAnd(std::move(combined), std::move(predicate));
  }
  return combined;
}

std::vector<RelationshipConnection> CollectConnectionsFromPattern(
    const Pattern &pattern) {
  std::vector<RelationshipConnection> connections;
  for (const auto &part : pattern.parts) {
    if (!part || !part->element) {
      continue;
    }
    for (const auto &chain_item : part->element->chain) {
      const auto &relationship_pattern = chain_item.first;
      if (!relationship_pattern || !relationship_pattern->detail) {
        continue;
      }
      const auto &detail = *relationship_pattern->detail;
      if (detail.variable.empty()) {
        continue;
      }

      RelationshipConnection connection;
      connection.is_group = detail.range.has_value();
      if (detail.range && detail.range->min && detail.range->min.value() == 0) {
        connection.can_be_empty = true;
      }
      connection.inner_relationships.push_back(
          SingleRelationship{detail.variable, detail.types});
      connections.push_back(std::move(connection));
    }
  }
  return connections;
}

std::unordered_set<std::string> CollectRelationshipNames(
    const std::vector<RelationshipConnection> &connections) {
  std::unordered_set<std::string> names;
  for (const auto &connection : connections) {
    for (const auto &relationship : connection.inner_relationships) {
      names.insert(relationship.name);
    }
  }
  return names;
}

const SingleRelationship &AsSingle(const RelationshipConnection &connection) {
  return connection.inner_relationships.front();
}

std::vector<std::string> CollectOverlappingNames(
    const std::vector<SingleRelationship> &lhs,
    const std::vector<SingleRelationship> &rhs) {
  std::vector<std::string> overlapping;
  for (const auto &left_rel : lhs) {
    const bool overlaps = std::any_of(
        rhs.begin(), rhs.end(), [&](const SingleRelationship &right_rel) {
          return !IsAlwaysDifferent(left_rel, right_rel);
        });
    if (overlaps) {
      overlapping.push_back(left_rel.name);
    }
  }
  return overlapping;
}

bool HasSameName(const std::vector<std::string> &lhs,
                 const std::vector<std::string> &rhs) {
  if (lhs.empty() || rhs.empty()) {
    return false;
  }
  std::unordered_set<std::string> lhs_set(lhs.begin(), lhs.end());
  return std::any_of(rhs.begin(), rhs.end(), [&](const std::string &name) {
    return lhs_set.find(name) != lhs_set.end();
  });
}

std::vector<std::string> RelationshipNames(
    const std::vector<SingleRelationship> &relationships) {
  std::vector<std::string> names;
  names.reserve(relationships.size());
  for (const auto &relationship : relationships) {
    names.push_back(relationship.name);
  }
  return names;
}

}  // namespace

void AddUniquenessPredicatesRewriter::Visit(Match &node) {
  ASTRewriter::Visit(node);
  if (!node.pattern) {
    return;
  }

  auto connections = CollectConnectionsFromPattern(*node.pattern);
  if (connections.empty()) {
    return;
  }

  auto used_names = CollectRelationshipNames(connections);
  next_temp_id_ = 0;
  auto fresh_temp_name = [&]() {
    for (;;) {
      std::string candidate = "__uniq_rel_" + std::to_string(next_temp_id_++);
      if (used_names.insert(candidate).second) {
        return candidate;
      }
    }
  };

  std::vector<std::unique_ptr<Expression>> predicates;

  for (size_t i = 0; i < connections.size(); ++i) {
    for (size_t j = i + 1; j < connections.size(); ++j) {
      const auto &lhs = connections[i];
      const auto &rhs = connections[j];

      if (!lhs.is_group && !rhs.is_group) {
        const auto &lhs_rel = AsSingle(lhs);
        const auto &rhs_rel = AsSingle(rhs);
        if (lhs_rel.name == rhs_rel.name) {
          predicates.push_back(MakeBooleanLiteral(false));
          continue;
        }
        if (!IsAlwaysDifferent(lhs_rel, rhs_rel)) {
          predicates.push_back(MakeNotEquals(lhs_rel.name, rhs_rel.name));
        }
        continue;
      }

      if (!lhs.is_group && rhs.is_group) {
        const auto &lhs_rel = AsSingle(lhs);
        std::vector<std::string> overlap_list;
        for (const auto &inner_rel : rhs.inner_relationships) {
          if (!IsAlwaysDifferent(lhs_rel, inner_rel)) {
            overlap_list.push_back(inner_rel.name);
          }
        }
        if (!overlap_list.empty()) {
          predicates.push_back(
              MakeNot(MakeIn(MakeVariable(lhs_rel.name),
                             MakeRelationshipListExpression(overlap_list))));
        }
        continue;
      }

      if (lhs.is_group && !rhs.is_group) {
        const auto &rhs_rel = AsSingle(rhs);
        std::vector<std::string> overlap_list;
        for (const auto &inner_rel : lhs.inner_relationships) {
          if (!IsAlwaysDifferent(rhs_rel, inner_rel)) {
            overlap_list.push_back(inner_rel.name);
          }
        }
        if (!overlap_list.empty()) {
          predicates.push_back(
              MakeNot(MakeIn(MakeVariable(rhs_rel.name),
                             MakeRelationshipListExpression(overlap_list))));
        }
        continue;
      }

      auto lhs_overlap = CollectOverlappingNames(lhs.inner_relationships,
                                                 rhs.inner_relationships);
      auto rhs_overlap = CollectOverlappingNames(rhs.inner_relationships,
                                                 lhs.inner_relationships);
      if (lhs_overlap.empty() || rhs_overlap.empty()) {
        continue;
      }

      if (HasSameName(lhs_overlap, rhs_overlap) &&
          !(lhs.can_be_empty || rhs.can_be_empty)) {
        predicates.push_back(MakeBooleanLiteral(false));
      } else {
        std::string outer_var = fresh_temp_name();
        auto in_predicate = MakeIn(MakeVariable(outer_var),
                                   MakeRelationshipListExpression(rhs_overlap));
        auto disjoint = std::make_unique<NoneQuantifier>();
        disjoint->variable = outer_var;
        disjoint->list_expr = MakeRelationshipListExpression(lhs_overlap);
        disjoint->predicate = std::move(in_predicate);
        predicates.push_back(std::move(disjoint));
      }
    }
  }

  for (const auto &connection : connections) {
    if (!connection.is_group) {
      continue;
    }
    const auto group_names = RelationshipNames(connection.inner_relationships);
    std::string all_var = fresh_temp_name();
    std::string single_var = fresh_temp_name();

    auto equals = std::make_unique<ComparisonExpression>();
    equals->left = MakeVariable(all_var);
    equals->op = "=";
    equals->right = MakeVariable(single_var);

    auto single = std::make_unique<SingleQuantifier>();
    single->variable = single_var;
    single->list_expr = MakeRelationshipListExpression(group_names);
    single->predicate = std::move(equals);

    auto all = std::make_unique<AllQuantifier>();
    all->variable = all_var;
    all->list_expr = MakeRelationshipListExpression(group_names);
    all->predicate = std::move(single);
    predicates.push_back(std::move(all));
  }

  if (predicates.empty()) {
    return;
  }

  auto uniqueness_predicate = CombinePredicates(predicates);
  node.where = MakeAnd(std::move(uniqueness_predicate), std::move(node.where));
}

}  // namespace ast
