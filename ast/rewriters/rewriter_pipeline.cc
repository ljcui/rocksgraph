#include "rewriter_pipeline.h"

#include <utility>

#include "rewriter_registry.h"

namespace ast {

RewriterPipeline::RewriterPipeline(
    std::vector<std::unique_ptr<ASTRewriter>> rewriters)
    : rewriters_(std::move(rewriters)) {}

void RewriterPipeline::Add(std::unique_ptr<ASTRewriter> rewriter) {
  rewriters_.push_back(std::move(rewriter));
}

void RewriterPipeline::Run(ASTNode &node) {
  for (const auto &rewriter : rewriters_) {
    if (rewriter) {
      rewriter->Rewrite(node);
    }
  }
}

RewriterPipeline makeDefaultRewriterPipeline() {
  return RewriterPipeline(makeDefaultRewriters());
}

void applyDefaultRewriters(ASTNode &node) {
  auto pipeline = makeDefaultRewriterPipeline();
  pipeline.Run(node);
}

}  // namespace ast
