#include "parenthesized_expression_rewriter.h"

namespace ast {

void ParenthesizedExpressionRewriter::RewriteExpression(
    std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::RewriteExpression(expr);
  while (expr && expr->Is(ASTNodeType::kParenthesizedExpression)) {
    auto *paren = CastAst<ParenthesizedExpression>(expr.get());
    if (!paren->expr) {
      return;
    }
    expr = std::move(paren->expr);
  }
}

}  // namespace ast
