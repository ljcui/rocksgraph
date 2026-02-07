#pragma once

#include "../ast_rewriter.h"

namespace ast {

class PatternPredicateNormalizationRewriter : public ASTRewriter {
 protected:
  void Visit(Match &node) override;
};

}  // namespace ast
