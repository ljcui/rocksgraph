#pragma once

#include "../ast_rewriter.h"

namespace ast {

class CountStarRewriter : public ASTRewriter {
 protected:
  void rewriteExpression(std::unique_ptr<Expression> &expr) override;
};

}  // namespace ast
