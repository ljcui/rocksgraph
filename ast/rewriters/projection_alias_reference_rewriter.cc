#include "projection_alias_reference_rewriter.h"

#include <string>
#include <utility>
#include <vector>

#include "../ast_equal.h"

namespace ast {
namespace {

struct AliasEntry {
  const std::string *alias = nullptr;
  const Expression *expression = nullptr;
};

std::vector<AliasEntry> ProjectionAliases(const ProjectionBody &body) {
  std::vector<AliasEntry> aliases;
  aliases.reserve(body.items.size());
  for (const auto &item : body.items) {
    if (!item || item->alias.empty() || !item->expression) {
      continue;
    }
    aliases.push_back(AliasEntry{&item->alias, item->expression.get()});
  }
  return aliases;
}

class AliasExpressionRewriter final : public ASTRewriter {
 public:
  explicit AliasExpressionRewriter(const std::vector<AliasEntry> &aliases)
      : aliases_(aliases) {}

  void RewriteAliasReferences(std::unique_ptr<Expression> &expr) {
    RewriteExpression(expr);
  }

 protected:
  void RewriteExpression(std::unique_ptr<Expression> &expr) override {
    if (!expr || IsAliasVariable(*expr)) {
      return;
    }
    for (const auto &alias : aliases_) {
      if (alias.alias != nullptr && alias.expression != nullptr &&
          ASTEqual::Equal(expr.get(), alias.expression)) {
        auto variable = std::make_unique<Variable>();
        variable->name = *alias.alias;
        expr = std::move(variable);
        return;
      }
    }
    ASTRewriter::RewriteExpression(expr);
  }

  // Variables introduced by these expressions shadow variables outside the
  // expression, so projection aliases must not be substituted inside them.
  void Visit(ListComprehension &node) override { (void)node; }
  void Visit(PatternComprehension &node) override { (void)node; }
  void Visit(AllQuantifier &node) override { (void)node; }
  void Visit(AnyQuantifier &node) override { (void)node; }
  void Visit(NoneQuantifier &node) override { (void)node; }
  void Visit(SingleQuantifier &node) override { (void)node; }
  void Visit(ExistentialSubquery &node) override { (void)node; }

 private:
  [[nodiscard]] bool IsAliasVariable(const Expression &expression) const {
    if (!expression.Is(ASTNodeType::kVariable)) {
      return false;
    }
    const auto *variable = CastAst<Variable>(&expression);
    for (const auto &alias : aliases_) {
      if (alias.alias != nullptr && variable->name == *alias.alias) {
        return true;
      }
    }
    return false;
  }

  const std::vector<AliasEntry> &aliases_;
};

}  // namespace

void ProjectionAliasReferenceRewriter::Visit(ProjectionBody &node) {
  ASTRewriter::Visit(node);
  if (node.items.empty() || node.order_by.empty()) {
    return;
  }

  const std::vector<AliasEntry> aliases = ProjectionAliases(node);
  if (aliases.empty()) {
    return;
  }

  AliasExpressionRewriter alias_rewriter(aliases);
  for (const auto &sort_item : node.order_by) {
    if (!sort_item || !sort_item->expression) {
      continue;
    }
    alias_rewriter.RewriteAliasReferences(sort_item->expression);
  }
}

void ProjectionAliasReferenceRewriter::Visit(With &node) {
  ASTRewriter::Visit(node);
  if (!node.body || !node.where) {
    return;
  }

  const std::vector<AliasEntry> aliases = ProjectionAliases(*node.body);
  if (aliases.empty()) {
    return;
  }

  AliasExpressionRewriter(aliases).RewriteAliasReferences(node.where);
}

}  // namespace ast
