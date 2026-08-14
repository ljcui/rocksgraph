#pragma once

#include <unordered_set>
#include <vector>

#include "../ast_rewriter.h"

namespace ast {

class ReturnStarRewriter : public ASTRewriter {
 public:
  void Rewrite(ASTNode &node);

 protected:
  void Visit(SinglePartQuery &node) override;
  void Visit(MultiPartQuery &node) override;
  void Visit(With &node) override;
  void Visit(ProjectionBody &node) override;

 private:
  struct Scope {
    std::vector<std::string> order;
    std::unordered_set<std::string> names;

    void Add(const std::string &name);
  };

  [[nodiscard]] const Scope &CurrentScope() const;
  Scope ScopeFromProjection(const ProjectionBody &body,
                            const Scope &fallback) const;
  void ExpandStar(ProjectionBody &body);

  void CollectFromReadingClause(const ReadingClause &clause,
                                Scope &scope) const;
  void CollectFromUpdatingClause(const UpdatingClause &clause,
                                 Scope &scope) const;
  void CollectFromPattern(const Pattern &pattern, Scope &scope) const;
  void CollectFromPatternPart(const PatternPart &part, Scope &scope) const;
  void CollectFromPatternElement(const PatternElement &element,
                                 Scope &scope) const;
  void CollectFromRelationshipsPattern(const RelationshipsPattern &pattern,
                                       Scope &scope) const;
  static void CollectFromNodePattern(const NodePattern &node, Scope &scope);
  static void CollectFromRelationshipDetail(const RelationshipDetail &detail,
                                            Scope &scope);
  static void CollectFromProjectionItem(const ProjectionItem &item,
                                        Scope &scope);

  std::vector<Scope> scope_stack_;
};

}  // namespace ast
