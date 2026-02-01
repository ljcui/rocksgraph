#include "parenthesized_expression_rewriter.h"

namespace ast {

void ParenthesizedExpressionRewriter::rewriteExpression(
    std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::rewriteExpression(expr);
  while (auto *paren = dynamic_cast<ParenthesizedExpression *>(expr.get())) {
    if (!paren->expr) {
      return;
    }
    expr = std::move(paren->expr);
  }
}

}  // namespace ast
