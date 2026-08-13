#pragma once

#include <functional>

#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "storage/storage.h"

namespace rg {

using WritePlanExecutor =
    std::function<QueryRows(const ir::LogicalPlan &, const QueryRows &)>;

class WriteExecutor final {
 public:
  explicit WriteExecutor(Storage *storage) : storage_(storage) {}

  [[nodiscard]] QueryRows Execute(const ir::CreateNodePlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::CreateRelationshipPlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::MergePlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::SetPropertyPlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::SetPropertiesPlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::SetLabelsPlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::RemovePropertyPlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::RemoveLabelsPlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::DeletePlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::DetachDeletePlan &plan,
                                  const QueryRows &input,
                                  const WritePlanExecutor &execute_plan) const;

 private:
  [[nodiscard]] QueryRows ExecuteDelete(
      const ir::LogicalPlan &plan,
      const std::vector<const ast::Expression *> &expressions,
      const QueryRows &input, const WritePlanExecutor &execute_plan,
      bool detach) const;
  [[nodiscard]] QueryRow ExecuteMergeCreate(const ir::CreatePattern &pattern,
                                            QueryRow row) const;
  void ExecuteSetPatterns(const std::vector<ir::SetMutatingPattern> &patterns,
                          QueryRow *row) const;
  void ApplySetProperty(const ir::SetMutatingPattern &pattern,
                        QueryRow *row) const;
  void ApplySetProperties(const ir::SetMutatingPattern &pattern, QueryRow *row,
                          bool include_existing) const;
  void ApplySetLabels(const ir::SetMutatingPattern &pattern,
                      QueryRow *row) const;
  [[nodiscard]] Storage &WritableStorage() const;

  Storage *storage_ = nullptr;
};

}  // namespace rg
