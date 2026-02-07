#pragma once

#include <vector>

#include "ast_walker.h"

namespace ast {

// ASTRewriter extends ASTWalker for in-place AST mutations.
// Override rewriteExpression when you need to replace an expression node.
class ASTRewriter : public ASTWalker {
 public:
  void Rewrite(ASTNode &node);

 protected:
  void RewriteMaybe(std::unique_ptr<Expression> &ptr) { WalkMaybe(ptr); }

  template <typename T>
  void RewriteMaybe(std::unique_ptr<T> &ptr) {
    WalkMaybe(ptr);
  }

  template <typename T>
  void RewriteList(std::vector<std::unique_ptr<T>> &list) {
    WalkList(list);
  }

  virtual void RewriteExpression(std::unique_ptr<Expression> &expr);

  void WalkExpression(std::unique_ptr<Expression> &expr) override;
};

}  // namespace ast
