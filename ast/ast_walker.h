#pragma once

#include <vector>

#include "ast_node.h"

namespace ast {

// ASTWalker provides a pure traversal over the AST without modifying it.
// Subclasses override visit methods to observe or collect information.
class ASTWalker : public ASTVisitor {
 public:
  void Walk(ASTNode &node);

 protected:
  void WalkMaybe(std::unique_ptr<Expression> &ptr) {
    if (ptr) {
      WalkExpression(ptr);
    }
  }

  template <typename T>
  void WalkMaybe(std::unique_ptr<T> &ptr) {
    if (ptr) {
      ptr->Accept(*this);
    }
  }

  template <typename T>
  void WalkList(std::vector<std::unique_ptr<T>> &list) {
    for (auto &item : list) {
      WalkMaybe(item);
    }
  }

  virtual void WalkExpression(std::unique_ptr<Expression> &expr);

#define AST_VISITOR_NODE(node_type) void Visit(node_type &node) override;
#include "ast_visitor_node_list.def"
#undef AST_VISITOR_NODE
};

}  // namespace ast
