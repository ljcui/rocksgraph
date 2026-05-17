#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast_node.h"
#include "ir/planner_query.h"

namespace ast {
class SemanticTable;
}

namespace ir {

void FinalizePlannerQueryArguments(PlannerQuery &query,
                                   const ast::SemanticTable &semantic_table);

std::unordered_set<std::string> SinglePlannerQueryOutputSymbols(
    const SinglePlannerQuery &query);

std::vector<std::string> SinglePlannerQueryOutputAliases(
    const SinglePlannerQuery &query);

std::vector<std::string> PlannerQueryOutputAliases(const PlannerQuery &query);

std::vector<UnionPlannerQuery::UnionMapping> BuildUnionMappings(
    const std::vector<std::string> &lhs_columns,
    const std::vector<std::string> &rhs_columns);

}  // namespace ir
