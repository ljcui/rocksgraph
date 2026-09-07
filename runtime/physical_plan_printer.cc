#include "runtime/physical_plan_printer.h"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast/expression_to_string.h"
#include "common/exception.h"

namespace rg {
namespace {

std::string FormatNumber(double value) {
  std::ostringstream out;
  out << std::setprecision(6) << std::defaultfloat << value;
  return out.str();
}

std::string FormatOrdering(const std::vector<ir::LogicalSortItem> &items) {
  std::ostringstream out;
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index > 0) {
      out << ", ";
    }
    const auto &item = items[index];
    out << (item.expression == nullptr
                ? "null"
                : ast::ExpressionToString(*item.expression))
        << ' ' << ir::ToString(item.direction);
  }
  return out.str();
}

std::string_view SlotKindName(SlotKind kind) {
  switch (kind) {
    case SlotKind::kNode:
      return "node";
    case SlotKind::kRelationship:
      return "relationship";
    case SlotKind::kReference:
      return "reference";
  }
  THROW(common::InternalError, "unknown slot kind");
}

std::string FormatSlots(const SlotConfiguration &slots) {
  std::ostringstream out;
  for (std::size_t index = 0; index < slots.Columns().size(); ++index) {
    if (index > 0) {
      out << ", ";
    }
    const std::string &column = slots.Columns()[index];
    const Slot &slot = slots.At(column);
    out << column << ':' << SlotKindName(slot.kind) << '@' << slot.offset;
    if (slot.nullable) {
      out << '?';
    }
  }
  return out.str();
}

class PhysicalPlanPrinter final {
 public:
  explicit PhysicalPlanPrinter(std::ostream &out) : out_(out) {}

  void Print(const PhysicalPlanNode &node) { PrintNode(node); }

 private:
  void PrintNode(const PhysicalPlanNode &node) {
    CHECK(node.logical != nullptr && node.output_slots != nullptr,
          common::InternalError, "physical plan node is incomplete");
    std::string line = node.type == PhysicalOperatorType::kLogical
                           ? std::string(node.logical->Name())
                           : std::string(ToString(node.type));
    std::string details = node.logical->Details();
    if (!details.empty()) {
      line.append(" [").append(details).append("]");
    }

    std::vector<std::string> metadata;
    metadata.push_back("id=" + std::to_string(node.id));
    if (node.type != PhysicalOperatorType::kLogical) {
      metadata.push_back("logical=" + std::string(node.logical->Name()));
    }
    metadata.push_back("exec=" + std::string(ToString(node.execution_kind)));
    if (node.logical->EstimatedRows().has_value()) {
      metadata.push_back("rows=" +
                         FormatNumber(*node.logical->EstimatedRows()));
    }
    if (node.logical->Cost().has_value()) {
      metadata.push_back("cost=" + FormatNumber(*node.logical->Cost()));
    }
    if (!node.provided_order.empty()) {
      metadata.push_back("order=[" + FormatOrdering(node.provided_order) + "]");
    }
    if (node.partial_sort_prefix > 0) {
      metadata.push_back("prefix=" + std::to_string(node.partial_sort_prefix));
    }
    if (node.partial_top_n_prefix > 0) {
      metadata.push_back("prefix=" + std::to_string(node.partial_top_n_prefix));
    }
    if (node.logical->Type() == ir::LogicalPlanNodeType::kValueHashJoin) {
      metadata.push_back(
          std::string("build=") +
          (node.value_hash_join_build_child == 0 ? "left" : "right"));
    }
    metadata.push_back("slots=[" + FormatSlots(*node.output_slots) + "]");

    line.append(" {");
    for (std::size_t index = 0; index < metadata.size(); ++index) {
      if (index > 0) {
        line.append(", ");
      }
      line.append(metadata[index]);
    }
    line.push_back('}');
    WriteLine(line);

    ++indent_;
    for (const auto &child : node.children) {
      CHECK(child != nullptr, common::InternalError,
            "physical plan child is null");
      PrintNode(*child);
    }
    --indent_;
  }

  void WriteLine(const std::string &line) {
    for (std::size_t index = 0; index < indent_; ++index) {
      out_ << "  ";
    }
    out_ << line << '\n';
  }

  std::ostream &out_;
  std::size_t indent_ = 0;
};

}  // namespace

void PrintPhysicalPlan(const PhysicalPlan &plan, std::ostream &out) {
  PhysicalPlanPrinter(out).Print(plan.Root());
}

std::string PhysicalPlanToString(const PhysicalPlan &plan) {
  std::ostringstream out;
  PrintPhysicalPlan(plan, out);
  return out.str();
}

}  // namespace rg
