#pragma once

#include <string>

#include "ast_node.h"

namespace ast {

// Convert AST back to a Cypher string.
std::string toCypher(ASTNode &node);

}  // namespace ast
