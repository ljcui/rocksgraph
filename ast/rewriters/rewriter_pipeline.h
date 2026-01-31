#pragma once

#include <memory>
#include <vector>

#include "../ast_rewriter.h"

namespace ast {

class RewriterPipeline {
 public:
  RewriterPipeline() = default;
  explicit RewriterPipeline(
      std::vector<std::unique_ptr<ASTRewriter>> rewriters);

  void add(std::unique_ptr<ASTRewriter> rewriter);
  void run(ASTNode &node);

 private:
  std::vector<std::unique_ptr<ASTRewriter>> rewriters_;
};

RewriterPipeline makeDefaultRewriterPipeline();
void applyDefaultRewriters(ASTNode &node);

}  // namespace ast
