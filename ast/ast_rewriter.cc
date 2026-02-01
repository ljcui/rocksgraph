#include "ast_rewriter.h"

namespace ast {

void ASTRewriter::rewrite(ASTNode &node) { walk(node); }

void ASTRewriter::rewriteExpression(std::unique_ptr<Expression> &expr) {
  if (expr) {
    expr->accept(*this);
  }
}

void ASTRewriter::walkExpression(std::unique_ptr<Expression> &expr) {
  rewriteExpression(expr);
}

}  // namespace ast
