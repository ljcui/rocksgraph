#include "ast_const_walker.h"

namespace ast {

void ASTConstWalker::Walk(const ASTNode &node) { node.Accept(*this); }

#define AST_WALKER_CLASS ASTConstWalker
#define AST_WALKER_PARAM(type) const type &
#include "ast_walker_visit_impl.inc"
#undef AST_WALKER_PARAM
#undef AST_WALKER_CLASS

}  // namespace ast
