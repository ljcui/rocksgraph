#pragma once

#include <memory>

#include "ast.h"

namespace ast {

std::unique_ptr<Expression> cloneExpression(const Expression &expr);

}  // namespace ast
