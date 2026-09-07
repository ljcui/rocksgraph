#pragma once

#include "../ast_rewriter.h"

namespace ast {

// Rewrites container['key'] to the statically addressable container.key form.
class LiteralDynamicPropertyRewriter : public ASTRewriter {
 protected:
  void RewriteExpression(std::unique_ptr<Expression> &expression) override;
};

}  // namespace ast
