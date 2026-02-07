#pragma once

#include "../ast_rewriter.h"

namespace ast {

class PatternPredicateRewriter : public ASTRewriter {
 protected:
  void RewriteExpression(std::unique_ptr<Expression> &expr) override;
};

}  // namespace ast
