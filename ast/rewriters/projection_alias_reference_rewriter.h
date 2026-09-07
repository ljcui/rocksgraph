#pragma once

#include "../ast_rewriter.h"

namespace ast {

class ProjectionAliasReferenceRewriter : public ASTRewriter {
 protected:
  void Visit(ProjectionBody &node) override;
  void Visit(With &node) override;
};

}  // namespace ast
