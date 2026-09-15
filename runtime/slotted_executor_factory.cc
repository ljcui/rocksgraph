#include <utility>

#include "runtime/slotted_executor_internal.h"

namespace rg::slotted {

std::unique_ptr<PullOperator> OperatorFactory::Build(
    const PhysicalPlanNode &node, std::optional<SlottedRow> argument) {
  switch (node.kind) {
    case PhysicalOperatorKind::kArgument:
    case PhysicalOperatorKind::kAllNodeScan:
    case PhysicalOperatorKind::kNodeByLabelScan:
    case PhysicalOperatorKind::kNodeIndexSeek:
    case PhysicalOperatorKind::kNodeIndexRangeSeek:
    case PhysicalOperatorKind::kRelationshipTypeScan:
    case PhysicalOperatorKind::kRelationshipIndexSeek:
    case PhysicalOperatorKind::kRelationshipIndexRangeSeek:
    case PhysicalOperatorKind::kNodeByIdSeek:
    case PhysicalOperatorKind::kRelationshipByIdSeek:
      return BuildLeafOperator(node, *state_, std::move(argument));

    case PhysicalOperatorKind::kVarExpand:
    case PhysicalOperatorKind::kPruningVarExpand:
    case PhysicalOperatorKind::kProjectEndpoints:
    case PhysicalOperatorKind::kOptionalExpand:
    case PhysicalOperatorKind::kExpand:
    case PhysicalOperatorKind::kExpandInto: {
      CHECK(node.children.size() == 1, common::InternalError,
            std::string(ToString(node.kind)) +
                " physical node must have one child");
      return BuildExpandOperator(node, *state_,
                                 Build(*node.children[0], std::move(argument)));
    }

    case PhysicalOperatorKind::kFilter:
    case PhysicalOperatorKind::kProjection:
    case PhysicalOperatorKind::kSkip:
    case PhysicalOperatorKind::kLimit:
    case PhysicalOperatorKind::kProduceResults:
    case PhysicalOperatorKind::kTopN:
    case PhysicalOperatorKind::kPartialTopN:
    case PhysicalOperatorKind::kOrderedDistinct:
    case PhysicalOperatorKind::kHashDistinct:
    case PhysicalOperatorKind::kOrderedAggregation:
    case PhysicalOperatorKind::kHashAggregation:
    case PhysicalOperatorKind::kPartialSort:
    case PhysicalOperatorKind::kFullSort:
    case PhysicalOperatorKind::kDelete:
    case PhysicalOperatorKind::kDetachDelete:
    case PhysicalOperatorKind::kWriteBarrier:
    case PhysicalOperatorKind::kPathBuild:
    case PhysicalOperatorKind::kProcedureCall:
    case PhysicalOperatorKind::kUnwind:
    case PhysicalOperatorKind::kAssertIsNode:
    case PhysicalOperatorKind::kCreateNode:
    case PhysicalOperatorKind::kCreateRelationship:
    case PhysicalOperatorKind::kSetProperty:
    case PhysicalOperatorKind::kSetProperties:
    case PhysicalOperatorKind::kSetLabels:
    case PhysicalOperatorKind::kRemoveProperty:
    case PhysicalOperatorKind::kRemoveLabels: {
      CHECK(node.children.size() == 1, common::InternalError,
            std::string(ToString(node.kind)) +
                " physical node must have one child");
      return BuildUnaryOperator(node, *state_,
                                Build(*node.children[0], std::move(argument)));
    }

    case PhysicalOperatorKind::kApply:
    case PhysicalOperatorKind::kOptionalApply:
    case PhysicalOperatorKind::kSemiApply:
    case PhysicalOperatorKind::kAntiSemiApply:
    case PhysicalOperatorKind::kLetSemiApply:
    case PhysicalOperatorKind::kSelectOrSemiApply:
    case PhysicalOperatorKind::kRollUpApply:
    case PhysicalOperatorKind::kMerge: {
      CHECK(node.children.size() == 2, common::InternalError,
            std::string(ToString(node.kind)) +
                " physical node must have two children");
      return BuildCorrelatedOperator(
          node, *state_, *this, Build(*node.children[0], std::move(argument)));
    }

    case PhysicalOperatorKind::kUnionAll:
    case PhysicalOperatorKind::kUnionDistinct:
    case PhysicalOperatorKind::kValueHashJoin:
    case PhysicalOperatorKind::kLeftOuterHashJoin:
    case PhysicalOperatorKind::kNodeHashJoin:
    case PhysicalOperatorKind::kCartesianProduct:
    case PhysicalOperatorKind::kPredicateJoin: {
      CHECK(node.children.size() == 2, common::InternalError,
            std::string(ToString(node.kind)) +
                " physical node must have two children");
      auto lhs = Build(*node.children[0], argument);
      auto rhs = Build(*node.children[1], std::move(argument));
      return BuildBinaryOperator(node, *state_, std::move(lhs), std::move(rhs));
    }
  }
  THROW(common::InternalError, "unsupported physical operator kind: " +
                                   std::string(ToString(node.kind)));
}

}  // namespace rg::slotted
