#include "anonymous_pattern_name_rewriter.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace ast {
namespace {

class NameCollector : public ASTRewriter {
 public:
  void Collect(ASTNode &node) {
    names.clear();
    rewrite(node);
  }

  std::unordered_set<std::string> names;

 private:
  void AddName(const std::string &name) {
    if (!name.empty()) {
      names.insert(name);
    }
  }

  void visit(Variable &node) override { AddName(node.name); }

  void visit(NodePattern &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(RelationshipDetail &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(PatternPart &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(ProjectionItem &node) override {
    AddName(node.alias);
    ASTRewriter::visit(node);
  }

  void visit(StandaloneCall &node) override {
    for (const auto &item : node.yield_items) {
      AddName(item.variable);
    }
    ASTRewriter::visit(node);
  }

  void visit(Unwind &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(InQueryCall &node) override {
    for (const auto &item : node.yield_items) {
      AddName(item.variable);
    }
    ASTRewriter::visit(node);
  }

  void visit(ListComprehension &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(PatternComprehension &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(AllQuantifier &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(AnyQuantifier &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(NoneQuantifier &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }

  void visit(SingleQuantifier &node) override {
    AddName(node.variable);
    ASTRewriter::visit(node);
  }
};

}  // namespace

void AnonymousPatternNameRewriter::visit(RegularQuery &node) {
  prepare(node);
  ASTRewriter::visit(node);
}

void AnonymousPatternNameRewriter::visit(StandaloneCall &node) {
  prepare(node);
  ASTRewriter::visit(node);
}

void AnonymousPatternNameRewriter::visit(Pattern &node) {
  prepare(node);
  ASTRewriter::visit(node);
}

void AnonymousPatternNameRewriter::visit(RelationshipsPattern &node) {
  prepare(node);
  ASTRewriter::visit(node);
}

void AnonymousPatternNameRewriter::visit(NodePattern &node) {
  if (!prepared_) {
    prepare(node);
  }
  if (node.variable.empty()) {
    node.variable = nextAnonymousName();
  }
  ASTRewriter::visit(node);
}

void AnonymousPatternNameRewriter::visit(RelationshipPattern &node) {
  if (!prepared_) {
    prepare(node);
  }
  if (!node.detail) {
    node.detail = std::make_unique<RelationshipDetail>();
  }
  ASTRewriter::visit(node);
}

void AnonymousPatternNameRewriter::visit(RelationshipDetail &node) {
  if (!prepared_) {
    prepare(node);
  }
  if (node.variable.empty()) {
    node.variable = nextAnonymousName();
  }
  ASTRewriter::visit(node);
}

void AnonymousPatternNameRewriter::prepare(ASTNode &node) {
  if (prepared_) {
    return;
  }
  NameCollector collector;
  collector.Collect(node);
  used_names_ = std::move(collector.names);
  prepared_ = true;
}

std::string AnonymousPatternNameRewriter::nextAnonymousName() {
  for (;;) {
    std::string name = "anon_" + std::to_string(next_id_++);
    if (used_names_.insert(name).second) {
      return name;
    }
  }
}

}  // namespace ast
