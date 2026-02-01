#pragma once

#include <string>

#include "ast.h"

namespace ast {

// Convert AST back to a Cypher string.
std::string toCypher(ASTNode &node);

}  // namespace ast
