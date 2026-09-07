#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "../ast_rewriter.h"

namespace ast {

// Evaluates subquery expressions immediately before supported updating clauses
// and replaces their update expressions with projected variables.
class MutatingSubqueryIsolationRewriter : public ASTRewriter {
 public:
  void Visit(RegularQuery &node) override;
  void Visit(UnionPart &node) override;

 private:
  void Prepare(ASTNode &node);
  void RewriteSingleQuery(std::unique_ptr<SingleQuery> &query);
  [[nodiscard]] std::unique_ptr<SingleQuery> RewriteSinglePart(
      std::unique_ptr<SinglePartQuery> query);
  void RewriteMultiPart(MultiPartQuery &query);
  [[nodiscard]] std::unique_ptr<With> IsolateExpressions(
      UpdatingClause &clause);
  [[nodiscard]] std::string NextAlias();

  std::unordered_set<std::string> used_names_;
  std::size_t next_alias_id_ = 0;
  bool prepared_ = false;
};

}  // namespace ast
