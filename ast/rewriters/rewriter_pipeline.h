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

  void Add(std::unique_ptr<ASTRewriter> rewriter);
  void Run(ASTNode &node);

 private:
  std::vector<std::unique_ptr<ASTRewriter>> rewriters_;
};

RewriterPipeline MakeDefaultRewriterPipeline();
void ApplyDefaultRewriters(ASTNode &node);

}  // namespace ast
