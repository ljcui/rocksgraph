#pragma once

#include "../ast_rewriter.h"

namespace ast {

class AddUniquenessPredicatesRewriter : public ASTRewriter {
 public:
  void visit(Match &node) override;

 private:
  int next_temp_id_ = 0;
};

}  // namespace ast
