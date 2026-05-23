#pragma once

#include <iosfwd>
#include <string>

#include "ir/logical_plan.h"

namespace ir {

void PrintLogicalPlan(const LogicalPlan &plan, std::ostream &out);
std::string LogicalPlanToString(const LogicalPlan &plan);

}  // namespace ir
