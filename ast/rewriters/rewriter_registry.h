#pragma once

#include <memory>
#include <vector>

#include "../ast_rewriter.h"

namespace ast {

std::vector<std::unique_ptr<ASTRewriter>> MakeDefaultRewriters();

}  // namespace ast
