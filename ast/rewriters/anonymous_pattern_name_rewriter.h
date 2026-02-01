#pragma once

#include <string>
#include <unordered_set>

#include "../ast_rewriter.h"

namespace ast {

class AnonymousPatternNameRewriter : public ASTRewriter {
 public:
  void visit(RegularQuery &node) override;
  void visit(StandaloneCall &node) override;
  void visit(Pattern &node) override;
  void visit(RelationshipsPattern &node) override;
  void visit(NodePattern &node) override;
  void visit(RelationshipPattern &node) override;
  void visit(RelationshipDetail &node) override;

 private:
  void prepare(ASTNode &node);
  std::string nextAnonymousName();

  bool prepared_ = false;
  std::unordered_set<std::string> used_names_;
  int next_id_ = 0;
};

}  // namespace ast
