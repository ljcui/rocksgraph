#pragma once

#include "ast_visitor.h"

namespace ast {

class ASTConstVisitor : public ASTVisitor {
 public:
  ~ASTConstVisitor() override = default;

#define DECLARE_CONST_VISIT(node_type) \
  virtual void Visit(const node_type &node) = 0;
#define AST_VISITOR_NODE(node_type) DECLARE_CONST_VISIT(node_type)
#include "ast_visitor_node_list.def"
#undef AST_VISITOR_NODE
#undef DECLARE_CONST_VISIT

#define DECLARE_BRIDGE_VISIT(node_type)          \
  void Visit(node_type &node) final {            \
    Visit(static_cast<const node_type &>(node)); \
  }
#define AST_VISITOR_NODE(node_type) DECLARE_BRIDGE_VISIT(node_type)
#include "ast_visitor_node_list.def"
#undef AST_VISITOR_NODE
#undef DECLARE_BRIDGE_VISIT
};

}  // namespace ast
