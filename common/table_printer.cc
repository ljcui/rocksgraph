#include "common/table_printer.h"

#include <algorithm>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>

namespace common {
namespace {

std::vector<std::string> SplitLines(std::string_view value) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (true) {
    const std::size_t end = value.find('\n', start);
    std::string line(value.substr(start, end - start));
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (end == std::string_view::npos) {
      return lines;
    }
    start = end + 1;
  }
}

void PrintBorder(std::ostream& output, const std::vector<std::size_t>& widths) {
  output << '+';
  for (const std::size_t width : widths) {
    output << std::string(width + 2, '-') << '+';
  }
  output << '\n';
}

}  // namespace

TablePrinter& TablePrinter::AddRow(std::vector<std::string> row) {
  column_count_ = std::max(column_count_, row.size());
  rows_.push_back(std::move(row));
  return *this;
}

TablePrinter& TablePrinter::AddRow(std::initializer_list<std::string> row) {
  return AddRow(std::vector<std::string>(row));
}

void TablePrinter::Print(std::ostream& output) const {
  if (rows_.empty() || column_count_ == 0) {
    return;
  }

  using CellLines = std::vector<std::string>;
  std::vector<std::vector<CellLines>> split_rows;
  split_rows.reserve(rows_.size());
  std::vector<std::size_t> widths(column_count_, 0);

  for (const auto& row : rows_) {
    std::vector<CellLines> split_row;
    split_row.reserve(column_count_);
    for (std::size_t column = 0; column < column_count_; ++column) {
      const std::string_view value = column < row.size()
                                         ? std::string_view(row[column])
                                         : std::string_view{};
      CellLines lines = SplitLines(value);
      for (const auto& line : lines) {
        widths[column] = std::max(widths[column], line.size());
      }
      split_row.push_back(std::move(lines));
    }
    split_rows.push_back(std::move(split_row));
  }

  for (std::size_t row = 0; row < split_rows.size(); ++row) {
    PrintBorder(output, widths);

    std::size_t row_height = 1;
    for (const auto& cell : split_rows[row]) {
      row_height = std::max(row_height, cell.size());
    }
    for (std::size_t line = 0; line < row_height; ++line) {
      output << '|';
      for (std::size_t column = 0; column < column_count_; ++column) {
        const auto& cell = split_rows[row][column];
        const std::string_view value = line < cell.size()
                                           ? std::string_view(cell[line])
                                           : std::string_view{};
        output << ' ' << value
               << std::string(widths[column] - value.size() + 1, ' ') << '|';
      }
      output << '\n';
    }
  }
  PrintBorder(output, widths);
}

std::string TablePrinter::ToString() const {
  std::ostringstream output;
  Print(output);
  return output.str();
}

std::ostream& operator<<(std::ostream& output, const TablePrinter& table) {
  table.Print(output);
  return output;
}

}  // namespace common
