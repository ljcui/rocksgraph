#pragma once

#include <memory>

#include "ast_node.h"

namespace ast {

std::unique_ptr<Expression> CloneExpression(const Expression &expr);

}  // namespace ast
