#include "pattern_predicate_normalization_rewriter.h"

#include <utility>

namespace ast {
namespace {

std::unique_ptr<Expression> makeVariable(const std::string &name) {
  auto node = std::make_unique<Variable>();
  node->name = name;
  return node;
}

std::unique_ptr<Expression> makeLabelPredicate(
    const std::string &name, std::vector<std::string> labels) {
  auto node = std::make_unique<LabelPredicateExpression>();
  node->expr = makeVariable(name);
  node->labels = std::move(labels);
  return node;
}

std::unique_ptr<Expression> makePropertyEquals(
    const std::string &name, const std::string &key,
    std::unique_ptr<Expression> value) {
  auto prop = std::make_unique<PropertyExpression>();
  prop->object = makeVariable(name);
  prop->property_key = key;

  auto node = std::make_unique<ComparisonExpression>();
  node->left = std::move(prop);
  node->op = "=";
  node->right = std::move(value);
  return node;
}

std::unique_ptr<Expression> combineAnd(std::unique_ptr<Expression> left,
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

std::unique_ptr<Expression> combineOr(std::unique_ptr<Expression> left,
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

void collectFromNodePattern(NodePattern &node,
                            std::vector<std::unique_ptr<Expression>> &preds) {
  if (node.variable.empty()) {
    return;
  }
  if (!node.labels.empty()) {
    preds.push_back(makeLabelPredicate(node.variable, std::move(node.labels)));
    node.labels.clear();
  }
  if (!node.properties || !node.properties->map) {
    return;
  }
  auto entries = std::move(node.properties->map->entries);
  for (auto &entry : entries) {
    preds.push_back(
        makePropertyEquals(node.variable, entry.first, std::move(entry.second)));
  }
  node.properties.reset();
}

void collectFromRelationshipDetail(
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
      type_predicate = makeLabelPredicate(detail.variable, std::move(label));
    } else {
      for (auto &type : detail.types) {
        std::vector<std::string> label;
        label.push_back(std::move(type));
        type_predicate =
            combineOr(std::move(type_predicate),
                      makeLabelPredicate(detail.variable, std::move(label)));
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
    preds.push_back(makePropertyEquals(detail.variable, entry.first,
                                       std::move(entry.second)));
  }
  detail.properties.reset();
}

void collectFromRelationshipPattern(
    RelationshipPattern &pattern,
    std::vector<std::unique_ptr<Expression>> &preds) {
  if (!pattern.detail) {
    return;
  }
  collectFromRelationshipDetail(*pattern.detail, preds);
}

void collectFromPatternElement(
    PatternElement &element,
    std::vector<std::unique_ptr<Expression>> &preds) {
  if (element.node_pattern) {
    collectFromNodePattern(*element.node_pattern, preds);
  }
  for (auto &link : element.chain) {
    if (link.first) {
      collectFromRelationshipPattern(*link.first, preds);
    }
    if (link.second) {
      collectFromNodePattern(*link.second, preds);
    }
  }
}

void collectFromPattern(Pattern &pattern,
                        std::vector<std::unique_ptr<Expression>> &preds) {
  for (auto &part : pattern.parts) {
    if (part && part->element) {
      collectFromPatternElement(*part->element, preds);
    }
  }
}

std::unique_ptr<Expression> combinePredicates(
    std::vector<std::unique_ptr<Expression>> &preds) {
  std::unique_ptr<Expression> combined;
  for (auto &pred : preds) {
    combined = combineAnd(std::move(combined), std::move(pred));
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
  collectFromPattern(*node.pattern, predicates);
  if (predicates.empty()) {
    return;
  }
  auto combined = combinePredicates(predicates);
  node.where = combineAnd(std::move(combined), std::move(node.where));
}

}  // namespace ast
