#include "ir/logical_plan_printer.h"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast/expression_to_string.h"
#include "common/exception.h"

namespace ir {
namespace {

std::string FormatNumber(double value) {
  std::ostringstream out;
  out << std::setprecision(6) << std::defaultfloat << value;
  return out.str();
}

std::string FormatOrdering(const std::vector<LogicalSortItem> &items) {
  std::ostringstream out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    const LogicalSortItem &item = items[i];
    out << (item.expression != nullptr
                ? ast::ExpressionToString(*item.expression)
                : "null")
        << " " << ToString(item.direction);
  }
  return out.str();
}

class LogicalPlanPrinter {
 public:
  LogicalPlanPrinter(std::ostream &out,
                     const LogicalPlanPrinterOptions &options)
      : out_(out), options_(options) {}

  void Print(const LogicalPlan &plan) { PrintPlan(plan); }

 private:
  void Line(const std::string &text) {
    for (int i = 0; i < indent_; ++i) {
      out_ << "  ";
    }
    out_ << text << '\n';
  }

  void Indent() { ++indent_; }
  void Dedent() {
    CHECK(indent_ > 0, common::InternalError,
          "logical plan printer indent underflow");
    --indent_;
  }

  void PrintPlan(const LogicalPlan &plan) {
    std::string line(plan.Name());
    const std::string details = plan.Details();
    if (!details.empty()) {
      line += " [";
      line += details;
      line += "]";
    }
    if (options_.include_metadata) {
      AppendMetadata(plan, &line);
    }
    Line(line);

    Indent();
    for (const auto &child : plan.Children()) {
      CHECK(child != nullptr, common::InternalError,
            "logical plan child is null");
      PrintPlan(*child);
    }
    Dedent();
  }

  void AppendMetadata(const LogicalPlan &plan, std::string *line) const {
    CHECK(line != nullptr, common::InternalError,
          "logical plan printer line is null");
    const LogicalPlanMetadata &metadata = plan.Metadata();
    if (!metadata.estimated_rows.has_value() && !metadata.cost.has_value() &&
        metadata.ordering.empty() && !metadata.distinct) {
      return;
    }

    std::vector<std::string> entries;
    if (metadata.estimated_rows.has_value()) {
      entries.push_back("rows=" + FormatNumber(*metadata.estimated_rows));
    }
    if (metadata.cost.has_value()) {
      entries.push_back("cost=" + FormatNumber(*metadata.cost));
    }
    if (!metadata.ordering.empty()) {
      entries.push_back("order=[" + FormatOrdering(metadata.ordering) + "]");
    }
    if (metadata.distinct) {
      entries.emplace_back("distinct=true");
    }

    line->append(" {");
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (i > 0) {
        line->append(", ");
      }
      line->append(entries[i]);
    }
    line->append("}");
  }

  std::ostream &out_;
  LogicalPlanPrinterOptions options_;
  int indent_ = 0;
};

}  // namespace

void PrintLogicalPlan(const LogicalPlan &plan, std::ostream &out) {
  PrintLogicalPlan(plan, out, LogicalPlanPrinterOptions{});
}

void PrintLogicalPlan(const LogicalPlan &plan, std::ostream &out,
                      const LogicalPlanPrinterOptions &options) {
  LogicalPlanPrinter printer(out, options);
  printer.Print(plan);
}

std::string LogicalPlanToString(const LogicalPlan &plan) {
  return LogicalPlanToString(plan, LogicalPlanPrinterOptions{});
}

std::string LogicalPlanToString(const LogicalPlan &plan,
                                const LogicalPlanPrinterOptions &options) {
  std::ostringstream out;
  PrintLogicalPlan(plan, out, options);
  return out.str();
}

}  // namespace ir
