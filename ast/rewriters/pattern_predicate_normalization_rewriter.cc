#include "pattern_predicate_normalization_rewriter.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>

namespace ast {
namespace {

class NameCollector final : public ASTWalker {
 public:
  std::unordered_set<std::string> Collect(ASTNode &node) {
    names_.clear();
    node.Accept(*this);
    return std::move(names_);
  }

 protected:
  void Visit(Variable &node) override { Add(node.name); }

  void Visit(NodePattern &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(RelationshipDetail &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(PatternPart &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(ProjectionItem &node) override {
    Add(node.alias);
    ASTWalker::Visit(node);
  }

  void Visit(StandaloneCall &node) override {
    for (const auto &item : node.yield_items) {
      Add(item.variable);
    }
    ASTWalker::Visit(node);
  }

  void Visit(Unwind &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(InQueryCall &node) override {
    for (const auto &item : node.yield_items) {
      Add(item.variable);
    }
    ASTWalker::Visit(node);
  }

  void Visit(ListComprehension &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(PatternComprehension &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(AllQuantifier &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(AnyQuantifier &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(NoneQuantifier &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(SingleQuantifier &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

 private:
  void Add(const std::string &name) {
    if (!name.empty()) {
      names_.insert(name);
    }
  }

  std::unordered_set<std::string> names_;
};

class PropertyVariableGenerator final {
 public:
  explicit PropertyVariableGenerator(ASTNode &node)
      : used_names_(NameCollector().Collect(node)) {}

  std::string Next() {
    for (;;) {
      std::string name = "__rel_prop_" + std::to_string(next_id_++);
      if (used_names_.insert(name).second) {
        return name;
      }
    }
  }

 private:
  std::unordered_set<std::string> used_names_;
  std::size_t next_id_ = 0;
};

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

std::unique_ptr<Expression> MakeAllPropertyEquals(
    const std::string &relationship_name, const std::string &item_name,
    const std::string &key, std::unique_ptr<Expression> value) {
  auto node = std::make_unique<AllQuantifier>();
  node->variable = item_name;
  node->list_expr = MakeVariable(relationship_name);
  node->predicate = MakePropertyEquals(item_name, key, std::move(value));
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
    RelationshipDetail &detail, std::vector<std::unique_ptr<Expression>> &preds,
    PropertyVariableGenerator *property_variables) {
  if (detail.variable.empty()) {
    return;
  }
  if (!detail.properties || !detail.properties->map) {
    return;
  }
  auto entries = std::move(detail.properties->map->entries);
  for (auto &entry : entries) {
    if (detail.range.has_value()) {
      const std::string item_name = property_variables->Next();
      preds.push_back(MakeAllPropertyEquals(
          detail.variable, item_name, entry.first, std::move(entry.second)));
    } else {
      preds.push_back(MakePropertyEquals(detail.variable, entry.first,
                                         std::move(entry.second)));
    }
  }
  detail.properties.reset();
}

void CollectFromRelationshipPattern(
    RelationshipPattern &pattern,
    std::vector<std::unique_ptr<Expression>> &preds,
    PropertyVariableGenerator *property_variables) {
  if (!pattern.detail) {
    return;
  }
  CollectFromRelationshipDetail(*pattern.detail, preds, property_variables);
}

void CollectFromPatternElement(PatternElement &element,
                               std::vector<std::unique_ptr<Expression>> &preds,
                               PropertyVariableGenerator *property_variables) {
  if (element.node_pattern) {
    CollectFromNodePattern(*element.node_pattern, preds);
  }
  for (auto &link : element.chain) {
    if (link.first) {
      CollectFromRelationshipPattern(*link.first, preds, property_variables);
    }
    if (link.second) {
      CollectFromNodePattern(*link.second, preds);
    }
  }
}

void CollectFromPattern(Pattern &pattern,
                        std::vector<std::unique_ptr<Expression>> &preds,
                        PropertyVariableGenerator *property_variables) {
  for (auto &part : pattern.parts) {
    if (part && part->element) {
      CollectFromPatternElement(*part->element, preds, property_variables);
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

void PatternPredicateNormalizationRewriter::Visit(Match &node) {
  ASTRewriter::Visit(node);
  if (!node.pattern) {
    return;
  }
  std::vector<std::unique_ptr<Expression>> predicates;
  PropertyVariableGenerator property_variables(node);
  CollectFromPattern(*node.pattern, predicates, &property_variables);
  if (predicates.empty()) {
    return;
  }
  auto combined = CombinePredicates(predicates);
  node.where = CombineAnd(std::move(combined), std::move(node.where));
}

}  // namespace ast
