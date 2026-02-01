#pragma once

#include "../ast_rewriter.h"

namespace ast {

class OrderByAliasRewriter : public ASTRewriter {
 protected:
  void visit(ProjectionBody &node) override;
};

}  // namespace ast
