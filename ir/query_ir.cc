#include "ir/query_ir.h"

#include <memory>

#include "ast/semantic_table.h"
#include "ir/query_ir_internal.h"

namespace ir {

std::unique_ptr<QueryIR> CreateQueryIR(const ast::Statement &statement) {
  const ast::SemanticTable semantic_table =
      ast::AnalyzeSemanticTable(statement);
  return BuildQueryIR(statement, semantic_table);
}

}  // namespace ir
