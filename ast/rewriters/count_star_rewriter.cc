#include "count_star_rewriter.h"

namespace ast {

void CountStarRewriter::RewriteExpression(std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::RewriteExpression(expr);
  if (dynamic_cast<CountStarExpression *>(expr.get()) == nullptr) {
    return;
  }

  auto function = std::make_unique<FunctionInvocation>();
  function->function_name = "count";
  function->distinct = false;
  auto literal = std::make_unique<IntegerLiteral>();
  literal->value = 1;
  function->arguments.push_back(std::move(literal));
  expr = std::move(function);
}

}  // namespace ast
