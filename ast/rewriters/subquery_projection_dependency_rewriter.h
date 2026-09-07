#pragma once

#include <optional>
#include <unordered_set>

#include "../ast_rewriter.h"
#include "../semantic_table.h"

namespace ast {

// Keeps correlated variables alive across WITH clauses inside subquery
// expressions.
class SubqueryProjectionDependencyRewriter : public ASTRewriter {
 public:
  void Visit(RegularQuery &node) override;
  void Visit(StandaloneCall &node) override;
  void Visit(ExistentialSubquery &node) override;

 private:
  void Prepare(const ASTNode &node);
  static void AddDependenciesToQuery(
      RegularQuery &query, const std::unordered_set<std::string> &dependencies);

  std::optional<SemanticTable> semantic_table_;
};

}  // namespace ast
