#pragma once

#include <iosfwd>
#include <string>

#include "runtime/physical_plan.h"

namespace runtime {

void PrintPhysicalPlan(const PhysicalPlan &plan, std::ostream &out);
[[nodiscard]] std::string PhysicalPlanToString(const PhysicalPlan &plan);

}  // namespace runtime
