#pragma once

#include "../ast_rewriter.h"

namespace ast {

class OrderByAliasRewriter : public ASTRewriter {
 protected:
  void Visit(ProjectionBody &node) override;
};

}  // namespace ast
