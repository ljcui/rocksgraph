#pragma once

#include <unordered_set>
#include <vector>

#include "../ast_rewriter.h"

namespace ast {

class ReturnStarRewriter : public ASTRewriter {
 public:
  void rewrite(ASTNode &node);

 protected:
  void visit(SinglePartQuery &node) override;
  void visit(MultiPartQuery &node) override;
  void visit(ProjectionBody &node) override;

 private:
  struct Scope {
    std::vector<std::string> order;
    std::unordered_set<std::string> names;

    void add(const std::string &name);
  };

  const Scope &currentScope() const;
  Scope scopeFromProjection(const ProjectionBody &body,
                            const Scope &fallback) const;
  void expandStar(ProjectionBody &body);

  void collectFromReadingClause(const ReadingClause &clause, Scope &scope) const;
  void collectFromUpdatingClause(const UpdatingClause &clause, Scope &scope) const;
  void collectFromPattern(const Pattern &pattern, Scope &scope) const;
  void collectFromPatternPart(const PatternPart &part, Scope &scope) const;
  void collectFromPatternElement(const PatternElement &element,
                                 Scope &scope) const;
  void collectFromRelationshipsPattern(const RelationshipsPattern &pattern,
                                       Scope &scope) const;
  void collectFromNodePattern(const NodePattern &node, Scope &scope) const;
  void collectFromRelationshipDetail(const RelationshipDetail &detail,
                                     Scope &scope) const;
  void collectFromProjectionItem(const ProjectionItem &item,
                                 Scope &scope) const;

  std::vector<Scope> scope_stack_;
};

}  // namespace ast
