#include "ir/logical_plan_printer.h"

#include <ostream>
#include <sstream>
#include <string>

#include "common/exception.h"

namespace ir {
namespace {

class LogicalPlanPrinter {
 public:
  explicit LogicalPlanPrinter(std::ostream &out) : out_(out) {}

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
    Line(line);

    Indent();
    for (const auto &child : plan.Children()) {
      CHECK(child != nullptr, common::InternalError,
            "logical plan child is null");
      PrintPlan(*child);
    }
    Dedent();
  }

  std::ostream &out_;
  int indent_ = 0;
};

}  // namespace

void PrintLogicalPlan(const LogicalPlan &plan, std::ostream &out) {
  LogicalPlanPrinter printer(out);
  printer.Print(plan);
}

std::string LogicalPlanToString(const LogicalPlan &plan) {
  std::ostringstream out;
  PrintLogicalPlan(plan, out);
  return out.str();
}

}  // namespace ir
