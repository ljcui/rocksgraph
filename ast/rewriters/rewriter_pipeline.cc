#include "rewriter_pipeline.h"

#include <utility>

#include "rewriter_registry.h"

namespace ast {

RewriterPipeline::RewriterPipeline(
    std::vector<std::unique_ptr<ASTRewriter>> rewriters)
    : rewriters_(std::move(rewriters)) {}

void RewriterPipeline::add(std::unique_ptr<ASTRewriter> rewriter) {
  rewriters_.push_back(std::move(rewriter));
}

void RewriterPipeline::run(ASTNode &node) {
  for (const auto &rewriter : rewriters_) {
    if (rewriter) {
      rewriter->rewrite(node);
    }
  }
}

RewriterPipeline makeDefaultRewriterPipeline() {
  return RewriterPipeline(makeDefaultRewriters());
}

void applyDefaultRewriters(ASTNode &node) {
  auto pipeline = makeDefaultRewriterPipeline();
  pipeline.run(node);
}

}  // namespace ast
