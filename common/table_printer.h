#pragma once

#include <cstddef>
#include <initializer_list>
#include <iosfwd>
#include <string>
#include <vector>

namespace common {

// A small dependency-free table formatter for terminal output.
//
// Cells are left aligned and may contain explicit '\n' line breaks. The
// formatter computes column widths from all rows before writing anything, so
// rows with different numbers of cells are supported as well.
class TablePrinter {
 public:
  TablePrinter() = default;

  TablePrinter& AddRow(std::vector<std::string> row);
  TablePrinter& AddRow(std::initializer_list<std::string> row);

  void Print(std::ostream& output) const;
  [[nodiscard]] std::string ToString() const;

 private:
  std::vector<std::vector<std::string>> rows_;
  std::size_t column_count_ = 0;
};

std::ostream& operator<<(std::ostream& output, const TablePrinter& table);

}  // namespace common
