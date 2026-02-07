#pragma once

#include <string>
#include <unordered_set>

#include "../ast_rewriter.h"

namespace ast {

class AnonymousPatternNameRewriter : public ASTRewriter {
 public:
  void Visit(RegularQuery &node) override;
  void Visit(StandaloneCall &node) override;
  void Visit(Pattern &node) override;
  void Visit(RelationshipsPattern &node) override;
  void Visit(NodePattern &node) override;
  void Visit(RelationshipPattern &node) override;
  void Visit(RelationshipDetail &node) override;

 private:
  void Prepare(ASTNode &node);
  std::string NextAnonymousName();

  bool prepared_ = false;
  std::unordered_set<std::string> used_names_;
  int next_id_ = 0;
};

}  // namespace ast
