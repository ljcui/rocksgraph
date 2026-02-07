#include "ast_walker.h"

namespace ast {

void ASTWalker::Walk(ASTNode &node) { node.Accept(*this); }

#define AST_WALKER_CLASS ASTWalker
#define AST_WALKER_PARAM(type) type &
#include "ast_walker_visit_impl.inc"
#undef AST_WALKER_PARAM
#undef AST_WALKER_CLASS

}  // namespace ast
