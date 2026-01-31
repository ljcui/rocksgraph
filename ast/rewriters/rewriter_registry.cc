#include "rewriter_registry.h"

#include "return_star_rewriter.h"

namespace ast {

std::vector<std::unique_ptr<ASTRewriter>> makeDefaultRewriters() {
  std::vector<std::unique_ptr<ASTRewriter>> rewriters;
  rewriters.emplace_back(std::make_unique<ReturnStarRewriter>());
  return rewriters;
}

}  // namespace ast
