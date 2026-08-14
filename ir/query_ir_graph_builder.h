#pragma once

#include <string>
#include <unordered_set>

#include "ast/ast_node.h"
#include "ir/query_ir.h"

namespace ast {
class SemanticTable;
}

namespace ir {

void AddPatternToQueryGraph(QueryGraph *query_graph,
                            const ast::Pattern &pattern);
void AddRelationshipsPatternToQueryGraph(
    QueryGraph *query_graph, const ast::RelationshipsPattern &pattern);

class QueryGraphBuilder {
 public:
  explicit QueryGraphBuilder(const ast::SemanticTable &semantic_table,
                             std::unordered_set<std::string> argument_ids = {});

  void BuildReadingClause(const ast::ReadingClause &clause);
  void BuildUpdatingClause(const ast::UpdatingClause &clause);

  [[nodiscard]] bool HasLocalWork() const;
  [[nodiscard]] bool HasOptionalMatches() const;
  [[nodiscard]] bool ContainsUpdates() const;
  [[nodiscard]] std::unordered_set<LogicalVariable> AvailableSymbols() const;

  void AddWhere(const ast::Expression *where);
  QueryGraph Release();

 private:
  void BuildMatch(const ast::Match &match);
  void BuildOptionalMatch(const ast::Match &match);
  void BuildRequiredMatch(const ast::Match &match);

  [[nodiscard]] std::unordered_set<LogicalVariable> CurrentNodeSymbols() const;
  [[nodiscard]] MutatingPattern BuildMutatingPattern(
      const ast::UpdatingClause &clause) const;
  [[nodiscard]] const ast::SemanticTable &SemanticTableRef() const;

  QueryGraph graph_;
  std::unordered_set<std::string> where_keys_;
  const ast::SemanticTable *semantic_table_ = nullptr;
};

}  // namespace ir
