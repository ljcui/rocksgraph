#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast_node.h"
#include "ir/query_ir.h"

namespace ast {
class SemanticTable;
}

namespace ir {

void FinalizeQueryIRArguments(QueryIR &query,
                              const ast::SemanticTable &semantic_table);

std::unordered_set<std::string> SingleQueryIROutputSymbols(
    const SingleQueryIR &query);

std::vector<std::string> SingleQueryIROutputAliases(const SingleQueryIR &query);

std::vector<std::string> QueryIROutputAliases(const QueryIR &query);

std::vector<UnionQueryIR::UnionMapping> BuildUnionMappings(
    const std::vector<std::string> &lhs_columns,
    const std::vector<std::string> &rhs_columns);

}  // namespace ir
