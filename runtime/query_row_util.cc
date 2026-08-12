#include "runtime/query_row_util.h"

#include <utility>

#include "common/exception.h"

namespace rg {

const Value &LookupQueryVariable(const QueryRow &row, const std::string &name) {
  const auto found = row.find(name);
  CHECK(found != row.end(), common::InvalidArgumentError,
        "variable is not bound: " + name);
  return found->second;
}

bool TryBindQueryVariable(QueryRow *row, const std::string &variable,
                          Value value) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (variable.empty()) {
    return true;
  }
  const auto found = row->find(variable);
  if (found == row->end()) {
    row->emplace(variable, std::move(value));
    return true;
  }
  return ValuesEqual(found->second, value);
}

bool MergeQueryRows(const QueryRow &left, const QueryRow &right,
                    QueryRow *out) {
  CHECK(out != nullptr, common::InternalError, "query row is null");
  *out = left;
  for (const auto &[key, value] : right) {
    if (!TryBindQueryVariable(out, key, value)) {
      return false;
    }
  }
  return true;
}

}  // namespace rg
