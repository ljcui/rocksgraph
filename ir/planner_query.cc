#include "ir/planner_query.h"

#include <memory>

#include "ast/semantic_table.h"
#include "ir/planner_query_internal.h"

namespace ir {

std::unique_ptr<PlannerQuery> CreatePlannerQuery(
    const ast::Statement &statement) {
  const ast::SemanticTable semantic_table =
      ast::AnalyzeSemanticTable(statement);
  return BuildPlannerQuery(statement, semantic_table);
}

}  // namespace ir
