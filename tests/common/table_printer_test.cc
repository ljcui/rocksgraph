#include "common/table_printer.h"

#include <gtest/gtest.h>

#include <sstream>

TEST(TablePrinterTest, PrintsAlignedRowsAndBorders) {
  common::TablePrinter table;
  table.AddRow({"name", "value"}).AddRow({"answer", "42"});

  EXPECT_EQ(table.ToString(),
            "+--------+-------+\n"
            "| name   | value |\n"
            "+--------+-------+\n"
            "| answer | 42    |\n"
            "+--------+-------+\n");
}

TEST(TablePrinterTest, PrintsExplicitMultilineCells) {
  common::TablePrinter table;
  table.AddRow({"name", "message"})
      .AddRow({"A", "first\nsecond"})
      .AddRow({"B", "last"});

  EXPECT_EQ(table.ToString(),
            "+------+---------+\n"
            "| name | message |\n"
            "+------+---------+\n"
            "| A    | first   |\n"
            "|      | second  |\n"
            "+------+---------+\n"
            "| B    | last    |\n"
            "+------+---------+\n");
}

TEST(TablePrinterTest, HandlesCarriageReturnsEmptyLinesAndUnevenRows) {
  common::TablePrinter table;
  table.AddRow({"left", "right"}).AddRow({"x\r\ny", ""}).AddRow({"only one"});

  EXPECT_EQ(table.ToString(),
            "+----------+-------+\n"
            "| left     | right |\n"
            "+----------+-------+\n"
            "| x        |       |\n"
            "| y        |       |\n"
            "+----------+-------+\n"
            "| only one |       |\n"
            "+----------+-------+\n");
}

TEST(TablePrinterTest, SupportsStreamInsertionAndEmptyTables) {
  common::TablePrinter empty;
  EXPECT_TRUE(empty.ToString().empty());

  common::TablePrinter table;
  table.AddRow({"one"});
  std::ostringstream output;
  output << table;
  EXPECT_EQ(output.str(), "+-----+\n| one |\n+-----+\n");
}
