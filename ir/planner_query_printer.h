#pragma once

#include <iosfwd>
#include <string>

#include "ir/planner_query.h"

namespace ir {

void PrintPlannerQuery(const PlannerQuery &query, std::ostream &out);
std::string PlannerQueryToString(const PlannerQuery &query);

}  // namespace ir
