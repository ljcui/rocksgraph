#pragma once

#include <string>

#include "ast_node.h"

namespace ast {

std::string ExpressionToString(const Expression &expr);
std::string UpdatingClauseToString(const UpdatingClause &clause);

}  // namespace ast
