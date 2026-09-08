#pragma once

#include <string>

namespace ast {

class Expression;

struct PrecomputedExpression {
  const Expression *expression = nullptr;
  std::string variable;
};

}  // namespace ast
