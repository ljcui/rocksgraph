#include "comparison_chain_rewriter.h"

#include <utility>

#include "../ast_clone.h"
#include "common/exception.h"

namespace ast {

void ComparisonChainRewriter::RewriteExpression(
    std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::RewriteExpression(expr);
  if (!expr->Is(ASTNodeType::kComparisonChainExpression)) {
    return;
  }
  auto *chain = CastAst<ComparisonChainExpression>(expr.get());
  CHECK(!chain->rights.empty(), common::InvalidArgumentError,
        "comparison chain has no right operand");
  auto left = std::move(chain->left);
  auto rights = std::move(chain->rights);
  std::unique_ptr<Expression> current_left = std::move(left);
  std::unique_ptr<Expression> combined;
  CHECK(current_left != nullptr, common::InvalidArgumentError,
        "comparison chain left operand is null");

  for (size_t i = 0; i < rights.size(); ++i) {
    auto &entry = rights[i];
    CHECK(entry.second != nullptr, common::InvalidArgumentError,
          "comparison chain right operand is null");
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
