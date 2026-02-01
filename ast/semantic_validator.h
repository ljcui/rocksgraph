#pragma once

#include <string>
#include <vector>

#include "ast.h"

namespace ast {

void validateStatement(ASTNode &node, std::vector<std::string> &errors);

}  // namespace ast
