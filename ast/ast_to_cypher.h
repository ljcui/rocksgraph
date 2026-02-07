#pragma once

#include <string>

#include "ast_node.h"

namespace ast {

// Convert AST back to a Cypher string.
std::string ToCypher(ASTNode &node);

}  // namespace ast
