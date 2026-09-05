#pragma once

#include <iosfwd>
#include <string>

#include "runtime/physical_plan.h"

namespace rg {

void PrintPhysicalPlan(const PhysicalPlan &plan, std::ostream &out);
[[nodiscard]] std::string PhysicalPlanToString(const PhysicalPlan &plan);

}  // namespace rg
