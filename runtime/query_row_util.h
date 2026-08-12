#pragma once

#include <string>

#include "runtime/query_row.h"
#include "value/value.h"

namespace rg {

[[nodiscard]] const Value &LookupQueryVariable(const QueryRow &row,
                                               const std::string &name);
[[nodiscard]] bool TryBindQueryVariable(QueryRow *row,
                                        const std::string &variable,
                                        Value value);
[[nodiscard]] bool MergeQueryRows(const QueryRow &left, const QueryRow &right,
                                  QueryRow *out);

}  // namespace rg
