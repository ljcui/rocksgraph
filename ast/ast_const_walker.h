#pragma once

#include <vector>

#include "ast_node.h"

namespace ast {

// ASTConstWalker provides a pure traversal over the AST without modifying it.
// Subclasses override visit methods to observe or collect information.
class ASTConstWalker : public ASTConstVisitor {
 public:
  void Walk(const ASTNode &node);

 protected:
  void WalkMaybe(const std::unique_ptr<Expression> &ptr) {
    if (ptr) {
      WalkExpression(ptr);
    }
  }

  template <typename T>
  void WalkMaybe(const std::unique_ptr<T> &ptr) {
    if (ptr) {
      const ASTNode &node = *ptr;
      node.Accept(*this);
    }
  }

  template <typename T>
  void WalkList(const std::vector<std::unique_ptr<T>> &list) {
    for (const auto &item : list) {
      WalkMaybe(item);
    }
  }

  virtual void WalkExpression(const std::unique_ptr<Expression> &expr);

#define AST_VISITOR_NODE(node_type) void Visit(const node_type &node) override;
#include "ast_visitor_node_list.def"
#undef AST_VISITOR_NODE
};

}  // namespace ast
