#pragma once

#include "../ast_rewriter.h"

namespace ast {

class PatternPredicateNormalizationRewriter : public ASTRewriter {
 protected:
  void visit(Match &node) override;
};

}  // namespace ast
