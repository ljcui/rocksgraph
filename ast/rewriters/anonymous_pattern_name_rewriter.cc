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
    Rewrite(node);
  }

  std::unordered_set<std::string> names;

 private:
  void AddName(const std::string &name) {
    if (!name.empty()) {
      names.insert(name);
    }
  }

  void Visit(Variable &node) override { AddName(node.name); }

  void Visit(NodePattern &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(RelationshipDetail &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(PatternPart &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(ProjectionItem &node) override {
    AddName(node.alias);
    ASTRewriter::Visit(node);
  }

  void Visit(StandaloneCall &node) override {
    for (const auto &item : node.yield_items) {
      AddName(item.variable);
    }
    ASTRewriter::Visit(node);
  }

  void Visit(Unwind &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(InQueryCall &node) override {
    for (const auto &item : node.yield_items) {
      AddName(item.variable);
    }
    ASTRewriter::Visit(node);
  }

  void Visit(ListComprehension &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(PatternComprehension &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(AllQuantifier &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(AnyQuantifier &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(NoneQuantifier &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }

  void Visit(SingleQuantifier &node) override {
    AddName(node.variable);
    ASTRewriter::Visit(node);
  }
};

}  // namespace

void AnonymousPatternNameRewriter::Visit(RegularQuery &node) {
  Prepare(node);
  ASTRewriter::Visit(node);
}

void AnonymousPatternNameRewriter::Visit(StandaloneCall &node) {
  Prepare(node);
  ASTRewriter::Visit(node);
}

void AnonymousPatternNameRewriter::Visit(Pattern &node) {
  Prepare(node);
  ASTRewriter::Visit(node);
}

void AnonymousPatternNameRewriter::Visit(RelationshipsPattern &node) {
  Prepare(node);
  ASTRewriter::Visit(node);
}

void AnonymousPatternNameRewriter::Visit(NodePattern &node) {
  if (!prepared_) {
    Prepare(node);
  }
  if (node.variable.empty()) {
    node.variable = NextAnonymousName();
  }
  ASTRewriter::Visit(node);
}

void AnonymousPatternNameRewriter::Visit(RelationshipPattern &node) {
  if (!prepared_) {
    Prepare(node);
  }
  if (!node.detail) {
    node.detail = std::make_unique<RelationshipDetail>();
  }
  ASTRewriter::Visit(node);
}

void AnonymousPatternNameRewriter::Visit(RelationshipDetail &node) {
  if (!prepared_) {
    Prepare(node);
  }
  if (node.variable.empty()) {
    node.variable = NextAnonymousName();
  }
  ASTRewriter::Visit(node);
}

void AnonymousPatternNameRewriter::Prepare(ASTNode &node) {
  if (prepared_) {
    return;
  }
  NameCollector collector;
  collector.Collect(node);
  used_names_ = std::move(collector.names);
  prepared_ = true;
}

std::string AnonymousPatternNameRewriter::NextAnonymousName() {
  for (;;) {
    std::string name = "anon_" + std::to_string(next_id_++);
    if (used_names_.insert(name).second) {
      return name;
    }
  }
}

}  // namespace ast
