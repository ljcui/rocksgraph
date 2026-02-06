#include "pattern_predicate_normalization_rewriter.h"

#include <utility>

namespace ast {
namespace {

std::unique_ptr<Expression> MakeVariable(const std::string &name) {
  auto node = std::make_unique<Variable>();
  node->name = name;
  return node;
}

std::unique_ptr<Expression> MakeLabelPredicate(
    const std::string &name, std::vector<std::string> labels) {
  auto node = std::make_unique<LabelPredicateExpression>();
  node->expr = MakeVariable(name);
  node->labels = std::move(labels);
  return node;
}

std::unique_ptr<Expression> MakePropertyEquals(
    const std::string &name, const std::string &key,
    std::unique_ptr<Expression> value) {
  auto prop = std::make_unique<PropertyExpression>();
  prop->object = MakeVariable(name);
  prop->property_key = key;

  auto node = std::make_unique<ComparisonExpression>();
  node->left = std::move(prop);
  node->op = "=";
  node->right = std::move(value);
  return node;
}

std::unique_ptr<Expression> CombineAnd(std::unique_ptr<Expression> left,
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

std::unique_ptr<Expression> CombineOr(std::unique_ptr<Expression> left,
                                      std::unique_ptr<Expression> right) {
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  auto node = std::make_unique<OrExpression>();
  node->left = std::move(left);
  node->right = std::move(right);
  return node;
}

void CollectFromNodePattern(NodePattern &node,
                            std::vector<std::unique_ptr<Expression>> &preds) {
  if (node.variable.empty()) {
    return;
  }
  if (!node.labels.empty()) {
    preds.push_back(MakeLabelPredicate(node.variable, std::move(node.labels)));
    node.labels.clear();
  }
  if (!node.properties || !node.properties->map) {
    return;
  }
  auto entries = std::move(node.properties->map->entries);
  for (auto &entry : entries) {
    preds.push_back(MakePropertyEquals(node.variable, entry.first,
                                       std::move(entry.second)));
  }
  node.properties.reset();
}

void CollectFromRelationshipDetail(
    RelationshipDetail &detail,
    std::vector<std::unique_ptr<Expression>> &preds) {
  if (detail.variable.empty()) {
    return;
  }
  if (!detail.types.empty()) {
    std::unique_ptr<Expression> type_predicate;
    if (detail.types.size() == 1) {
      std::vector<std::string> label;
      label.push_back(std::move(detail.types.front()));
      type_predicate = MakeLabelPredicate(detail.variable, std::move(label));
    } else {
      for (auto &type : detail.types) {
        std::vector<std::string> label;
        label.push_back(std::move(type));
        type_predicate =
            CombineOr(std::move(type_predicate),
                      MakeLabelPredicate(detail.variable, std::move(label)));
      }
    }
    detail.types.clear();
    if (type_predicate) {
      preds.push_back(std::move(type_predicate));
    }
  }
  if (!detail.properties || !detail.properties->map) {
    return;
  }
  auto entries = std::move(detail.properties->map->entries);
  for (auto &entry : entries) {
    preds.push_back(MakePropertyEquals(detail.variable, entry.first,
                                       std::move(entry.second)));
  }
  detail.properties.reset();
}

void CollectFromRelationshipPattern(
    RelationshipPattern &pattern,
    std::vector<std::unique_ptr<Expression>> &preds) {
  if (!pattern.detail) {
    return;
  }
  CollectFromRelationshipDetail(*pattern.detail, preds);
}

void CollectFromPatternElement(
    PatternElement &element, std::vector<std::unique_ptr<Expression>> &preds) {
  if (element.node_pattern) {
    CollectFromNodePattern(*element.node_pattern, preds);
  }
  for (auto &link : element.chain) {
    if (link.first) {
      CollectFromRelationshipPattern(*link.first, preds);
    }
    if (link.second) {
      CollectFromNodePattern(*link.second, preds);
    }
  }
}

void CollectFromPattern(Pattern &pattern,
                        std::vector<std::unique_ptr<Expression>> &preds) {
  for (auto &part : pattern.parts) {
    if (part && part->element) {
      CollectFromPatternElement(*part->element, preds);
    }
  }
}

std::unique_ptr<Expression> CombinePredicates(
    std::vector<std::unique_ptr<Expression>> &preds) {
  std::unique_ptr<Expression> combined;
  for (auto &pred : preds) {
    combined = CombineAnd(std::move(combined), std::move(pred));
  }
  return combined;
}

}  // namespace

void PatternPredicateNormalizationRewriter::visit(Match &node) {
  ASTRewriter::visit(node);
  if (!node.pattern) {
    return;
  }
  std::vector<std::unique_ptr<Expression>> predicates;
  CollectFromPattern(*node.pattern, predicates);
  if (predicates.empty()) {
    return;
  }
  auto combined = CombinePredicates(predicates);
  node.where = CombineAnd(std::move(combined), std::move(node.where));
}

}  // namespace ast
