#pragma once

#include <vector>

#include "ast_walker.h"

namespace ast {

// ASTRewriter extends ASTWalker for in-place AST mutations.
// Override rewriteExpression when you need to replace an expression node.
class ASTRewriter : public ASTWalker {
 public:
  void rewrite(ASTNode &node);

 protected:
  void rewriteMaybe(std::unique_ptr<Expression> &ptr) {
    walkMaybe(ptr);
  }

  template <typename T>
  void rewriteMaybe(std::unique_ptr<T> &ptr) {
    walkMaybe(ptr);
  }

  template <typename T>
  void rewriteList(std::vector<std::unique_ptr<T>> &list) {
    walkList(list);
  }

  virtual void rewriteExpression(std::unique_ptr<Expression> &expr);

  void walkExpression(std::unique_ptr<Expression> &expr) override;
};

}  // namespace ast
