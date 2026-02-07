#include "ast_rewriter.h"

namespace ast {

void ASTRewriter::Rewrite(ASTNode &node) { Walk(node); }

void ASTRewriter::RewriteExpression(std::unique_ptr<Expression> &expr) {
  if (expr) {
    expr->Accept(*this);
  }
}

void ASTRewriter::WalkExpression(std::unique_ptr<Expression> &expr) {
  RewriteExpression(expr);
}

}  // namespace ast
