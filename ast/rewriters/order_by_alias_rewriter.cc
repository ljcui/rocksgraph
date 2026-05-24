#include "order_by_alias_rewriter.h"

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

class AliasExpressionRewriter final : public ASTRewriter {
 public:
  explicit AliasExpressionRewriter(const std::vector<AliasEntry> &aliases)
      : aliases_(aliases) {}

  void RewriteOrderExpression(std::unique_ptr<Expression> &expr) {
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

void OrderByAliasRewriter::Visit(ProjectionBody &node) {
  ASTRewriter::Visit(node);
  if (node.items.empty() || node.order_by.empty()) {
    return;
  }

  std::vector<AliasEntry> aliases;
  aliases.reserve(node.items.size());
  for (const auto &item : node.items) {
    if (!item || item->alias.empty() || !item->expression) {
      continue;
    }
    aliases.push_back(AliasEntry{&item->alias, item->expression.get()});
  }
  if (aliases.empty()) {
    return;
  }

  AliasExpressionRewriter alias_rewriter(aliases);
  for (const auto &sort_item : node.order_by) {
    if (!sort_item || !sort_item->expression) {
      continue;
    }
    alias_rewriter.RewriteOrderExpression(sort_item->expression);
  }
}

}  // namespace ast
