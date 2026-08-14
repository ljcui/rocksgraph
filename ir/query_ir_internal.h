#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ast/ast_node.h"
#include "ir/query_ir.h"

namespace ast {
class SemanticTable;
}

namespace ir {

std::string Unsupported(std::string_view feature);
std::string UnsupportedInStage(std::string_view stage,
                               std::string_view feature);
std::string Missing(std::string_view subject);

const ast::Expression *UnwrapParenthesized(const ast::Expression *expression);
SingleQueryIR *LastQueryPart(SingleQueryIR *query);
const SingleQueryIR *LastQueryPart(const SingleQueryIR *query);
const ast::Variable *AsVariableExpression(const ast::Expression *expression);
const ast::PropertyExpression *AsPropertyExpression(
    const ast::Expression *expression);

PatternPropertyMap BuildPropertyMap(const ast::Properties *properties);

void AddCreatePatternNodeSymbols(std::unordered_set<std::string> *symbols,
                                 const CreatePattern &pattern);
void AddCreatePatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const CreatePattern &pattern);
void AddSetMutatingPatternDependencySymbols(
    std::unordered_set<std::string> *dependencies,
    const std::vector<SetMutatingPattern> &patterns);

std::unordered_set<std::string> MutatingPatternAvailableSymbols(
    const MutatingPattern &mutating_pattern);
std::unordered_set<std::string> MutatingPatternDependencies(
    const MutatingPattern &mutating_pattern);
std::unordered_set<std::string> QueryGraphLocalAvailableSymbols(
    const QueryGraph &query_graph);
std::unordered_set<std::string> QueryGraphAvailableSymbols(
    const QueryGraph &query_graph);

void AddSymbol(std::unordered_set<std::string> *symbols,
               const std::string &symbol);
void AddSymbols(std::unordered_set<std::string> *symbols,
                const std::unordered_set<std::string> &incoming);
std::unordered_set<std::string> IntersectSymbols(
    const std::unordered_set<std::string> &lhs,
    const std::unordered_set<std::string> &rhs);

bool StringEquals(const std::string &value, std::string_view expected);
bool StringVectorContains(const std::vector<std::string> &values,
                          std::string_view expected);
bool StringSetContains(const std::unordered_set<std::string> &values,
                       std::string_view expected);
bool StringSetEquals(const std::unordered_set<std::string> &lhs,
                     const std::unordered_set<std::string> &rhs);
bool IsPropertyPredicateKind(PredicateKind kind);
bool IsLowerBoundComparison(std::string_view op);
bool IsUpperBoundComparison(std::string_view op);
bool DependenciesMet(const std::unordered_set<std::string> &dependencies,
                     const std::unordered_set<std::string> &bound_symbols);
std::string PredicateKey(const Predicate &predicate);

void AddAssertIsNodeVariables(QueryGraph *query_graph);
void AddSelectionPredicates(const ast::Expression *where,
                            const ast::SemanticTable &semantic_table,
                            const QueryGraph *query_graph,
                            Selections *selections,
                            std::unordered_set<std::string> *selection_keys);

std::unique_ptr<QueryIR> BuildQueryIR(const ast::Statement &statement,
                                      const ast::SemanticTable &semantic_table);

}  // namespace ir
