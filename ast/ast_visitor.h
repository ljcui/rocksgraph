#pragma once

namespace ast {

#define AST_VISITOR_NODE(node_type) class node_type;
#include "ast_visitor_node_list.def"
#undef AST_VISITOR_NODE

class ASTVisitor {
 public:
  virtual ~ASTVisitor() = default;

#define AST_VISITOR_NODE(node_type) virtual void Visit(node_type& node) = 0;
#include "ast_visitor_node_list.def"
#undef AST_VISITOR_NODE
};

}  // namespace ast
