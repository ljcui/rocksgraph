#include "comparison_chain_rewriter.h"

#include <cassert>
#include <utility>

#include "../ast_clone.h"

namespace ast {

void ComparisonChainRewriter::RewriteExpression(
    std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::RewriteExpression(expr);
  auto *chain = dynamic_cast<ComparisonChainExpression *>(expr.get());
  if ((chain == nullptr) || chain->rights.empty()) {
    return;
  }
  auto left = std::move(chain->left);
  auto rights = std::move(chain->rights);
  std::unique_ptr<Expression> current_left = std::move(left);
  std::unique_ptr<Expression> combined;
  assert(current_left);

  for (size_t i = 0; i < rights.size(); ++i) {
    auto &entry = rights[i];
    assert(entry.second);
    auto comparison = std::make_unique<ComparisonExpression>();
    comparison->left = std::move(current_left);
    comparison->op = entry.first;
    if (i + 1 < rights.size()) {
      comparison->right = CloneExpression(*entry.second);
      current_left = std::move(entry.second);
    } else {
      comparison->right = std::move(entry.second);
    }
    if (!combined) {
      combined = std::move(comparison);
    } else {
      auto and_expr = std::make_unique<AndExpression>();
      and_expr->left = std::move(combined);
      and_expr->right = std::move(comparison);
      combined = std::move(and_expr);
    }
  }

  expr = std::move(combined);
}

}  // namespace ast
