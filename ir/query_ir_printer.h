#pragma once

#include <iosfwd>
#include <string>

#include "ir/query_ir.h"

namespace ir {

void PrintQueryIR(const QueryIR &query, std::ostream &out);
std::string QueryIRToString(const QueryIR &query);

}  // namespace ir
