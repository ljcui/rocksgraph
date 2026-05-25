#pragma once

#include <iosfwd>
#include <string>

#include "ir/logical_plan.h"

namespace ir {

struct LogicalPlanPrinterOptions {
  bool include_metadata = false;
};

void PrintLogicalPlan(const LogicalPlan &plan, std::ostream &out);
void PrintLogicalPlan(const LogicalPlan &plan, std::ostream &out,
                      const LogicalPlanPrinterOptions &options);
std::string LogicalPlanToString(const LogicalPlan &plan);
std::string LogicalPlanToString(const LogicalPlan &plan,
                                const LogicalPlanPrinterOptions &options);

}  // namespace ir
