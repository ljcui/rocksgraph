#include "parenthesized_expression_rewriter.h"

#include "common/exception.h"

namespace ast {

void ParenthesizedExpressionRewriter::RewriteExpression(
    std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::RewriteExpression(expr);
  while (expr && expr->Is(ASTNodeType::kParenthesizedExpression)) {
    auto *paren = CastAst<ParenthesizedExpression>(expr.get());
    CHECK(paren->expr != nullptr, common::InvalidArgumentError,
          "parenthesized expression is null");
    expr = std::move(paren->expr);
  }
}

}  // namespace ast
