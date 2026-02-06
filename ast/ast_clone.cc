#include "ast_clone.h"

#include <utility>

#include "common/exception.h"

namespace ast {

using common::InternalError;

namespace {

template <typename T, typename CloneFunc>
std::unique_ptr<T> CloneMaybe(const std::unique_ptr<T> &ptr,
                              CloneFunc clone_func) {
  if (!ptr) {
    return nullptr;
  }
  return clone_func(*ptr);
}

template <typename T, typename CloneFunc>
std::vector<std::unique_ptr<T>> CloneList(
    const std::vector<std::unique_ptr<T>> &list, CloneFunc clone_func) {
  std::vector<std::unique_ptr<T>> cloned;
  cloned.reserve(list.size());
  for (const auto &item : list) {
    cloned.push_back(CloneMaybe(item, clone_func));
  }
  return cloned;
}

std::unique_ptr<Expression> CloneExpressionImpl(const Expression &expr);
std::unique_ptr<RegularQuery> CloneRegularQuery(const RegularQuery &query);
std::unique_ptr<SingleQuery> CloneSingleQuery(const SingleQuery &query);
std::unique_ptr<SinglePartQuery> CloneSinglePartQuery(
    const SinglePartQuery &query);
std::unique_ptr<MultiPartQuery> CloneMultiPartQuery(
    const MultiPartQuery &query);
std::unique_ptr<UnionPart> CloneUnionPart(const UnionPart &part);
std::unique_ptr<Pattern> ClonePattern(const Pattern &pattern);
std::unique_ptr<PatternPart> ClonePatternPart(const PatternPart &part);
std::unique_ptr<PatternElement> ClonePatternElement(
    const PatternElement &element);
std::unique_ptr<RelationshipsPattern> CloneRelationshipsPattern(
    const RelationshipsPattern &pattern);
std::unique_ptr<NodePattern> CloneNodePattern(const NodePattern &node);
std::unique_ptr<RelationshipPattern> CloneRelationshipPattern(
    const RelationshipPattern &pattern);
std::unique_ptr<RelationshipDetail> CloneRelationshipDetail(
    const RelationshipDetail &detail);
std::unique_ptr<Properties> CloneProperties(const Properties &props);
std::unique_ptr<MapLiteral> CloneMapLiteral(const MapLiteral &map);
std::unique_ptr<ListLiteral> CloneListLiteral(const ListLiteral &list);
std::unique_ptr<ReadingClause> CloneReadingClause(const ReadingClause &clause);
std::unique_ptr<UpdatingClause> CloneUpdatingClause(
    const UpdatingClause &clause);
std::unique_ptr<Match> CloneMatch(const Match &match);
std::unique_ptr<Unwind> CloneUnwind(const Unwind &unwind);
std::unique_ptr<InQueryCall> CloneInQueryCall(const InQueryCall &call);
std::unique_ptr<Create> CloneCreate(const Create &create);
std::unique_ptr<Merge> CloneMerge(const Merge &merge);
std::unique_ptr<Delete> CloneDelete(const Delete &del);
std::unique_ptr<Set> CloneSet(const Set &set);
std::unique_ptr<SetItem> CloneSetItem(const SetItem &item);
std::unique_ptr<Remove> CloneRemove(const Remove &remove);
std::unique_ptr<RemoveItem> CloneRemoveItem(const RemoveItem &item);
std::unique_ptr<ProjectionBody> CloneProjectionBody(const ProjectionBody &body);
std::unique_ptr<ProjectionItem> CloneProjectionItem(const ProjectionItem &item);
std::unique_ptr<SortItem> CloneSortItem(const SortItem &item);
std::unique_ptr<With> CloneWith(const With &with_clause);
std::unique_ptr<Return> CloneReturn(const Return &return_clause);

std::unique_ptr<Expression> CloneExpressionPtr(
    const std::unique_ptr<Expression> &expr) {
  return CloneMaybe(expr, CloneExpressionImpl);
}

std::unique_ptr<Pattern> ClonePatternPtr(
    const std::unique_ptr<Pattern> &pattern) {
  return CloneMaybe(pattern, ClonePattern);
}

std::unique_ptr<RelationshipsPattern> CloneRelationshipsPatternPtr(
    const std::unique_ptr<RelationshipsPattern> &pattern) {
  return CloneMaybe(pattern, CloneRelationshipsPattern);
}

std::unique_ptr<Properties> ClonePropertiesPtr(
    const std::unique_ptr<Properties> &props) {
  return CloneMaybe(props, CloneProperties);
}

std::unique_ptr<Variable> CloneVariable(const Variable &var) {
  auto cloned = std::make_unique<Variable>();
  cloned->name = var.name;
  return cloned;
}

std::unique_ptr<Parameter> CloneParameter(const Parameter &param) {
  auto cloned = std::make_unique<Parameter>();
  cloned->name = param.name;
  return cloned;
}

template <typename T>
std::unique_ptr<T> CloneBinary(const T &node) {
  auto cloned = std::make_unique<T>();
  cloned->left = CloneExpressionPtr(node.left);
  cloned->right = CloneExpressionPtr(node.right);
  return cloned;
}

template <typename T>
std::unique_ptr<T> CloneUnary(const T &node) {
  auto cloned = std::make_unique<T>();
  cloned->operand = CloneExpressionPtr(node.operand);
  return cloned;
}

template <typename T>
std::unique_ptr<T> CloneQuantifier(const T &node) {
  auto cloned = std::make_unique<T>();
  cloned->variable = node.variable;
  cloned->list_expr = CloneExpressionPtr(node.list_expr);
  cloned->predicate = CloneExpressionPtr(node.predicate);
  return cloned;
}

std::unique_ptr<Expression> CloneExpressionImpl(const Expression &expr) {
  if (const auto *node = dynamic_cast<const OrExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const XorExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const AndExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const ComparisonExpression *>(&expr)) {
    auto cloned = std::make_unique<ComparisonExpression>();
    cloned->left = CloneExpressionPtr(node->left);
    cloned->op = node->op;
    cloned->right = CloneExpressionPtr(node->right);
    return cloned;
  }
  if (const auto *node =
          dynamic_cast<const ComparisonChainExpression *>(&expr)) {
    auto cloned = std::make_unique<ComparisonChainExpression>();
    cloned->left = CloneExpressionPtr(node->left);
    cloned->rights.reserve(node->rights.size());
    for (const auto &entry : node->rights) {
      cloned->rights.emplace_back(entry.first,
                                  CloneExpressionPtr(entry.second));
    }
    return cloned;
  }
  if (const auto *node = dynamic_cast<const AddExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const SubtractExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const MultiplyExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const DivideExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const ModuloExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const PowerExpression *>(&expr)) {
    return CloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const NotExpression *>(&expr)) {
    return CloneUnary(*node);
  }
  if (const auto *node = dynamic_cast<const UnaryPlusExpression *>(&expr)) {
    return CloneUnary(*node);
  }
  if (const auto *node = dynamic_cast<const UnaryMinusExpression *>(&expr)) {
    return CloneUnary(*node);
  }
  if (const auto *node =
          dynamic_cast<const StringPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<StringPredicateExpression>();
    cloned->left = CloneExpressionPtr(node->left);
    cloned->op = node->op;
    cloned->right = CloneExpressionPtr(node->right);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ListPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<ListPredicateExpression>();
    cloned->element = CloneExpressionPtr(node->element);
    cloned->list = CloneExpressionPtr(node->list);
    return cloned;
  }
  if (const auto *node =
          dynamic_cast<const LabelPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<LabelPredicateExpression>();
    cloned->expr = CloneExpressionPtr(node->expr);
    cloned->labels = node->labels;
    return cloned;
  }
  if (const auto *node = dynamic_cast<const NullPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<NullPredicateExpression>();
    cloned->operand = CloneExpressionPtr(node->operand);
    cloned->is_null = node->is_null;
    return cloned;
  }
  if (const auto *node = dynamic_cast<const BooleanLiteral *>(&expr)) {
    auto cloned = std::make_unique<BooleanLiteral>();
    cloned->value = node->value;
    return cloned;
  }
  if (const auto *node = dynamic_cast<const IntegerLiteral *>(&expr)) {
    auto cloned = std::make_unique<IntegerLiteral>();
    cloned->value = node->value;
    return cloned;
  }
  if (const auto *node = dynamic_cast<const DoubleLiteral *>(&expr)) {
    auto cloned = std::make_unique<DoubleLiteral>();
    cloned->value = node->value;
    return cloned;
  }
  if (const auto *node = dynamic_cast<const StringLiteral *>(&expr)) {
    auto cloned = std::make_unique<StringLiteral>();
    cloned->value = node->value;
    return cloned;
  }
  if (dynamic_cast<const NullLiteral *>(&expr) != nullptr) {
    return std::make_unique<NullLiteral>();
  }
  if (const auto *node = dynamic_cast<const ListLiteral *>(&expr)) {
    return CloneListLiteral(*node);
  }
  if (const auto *node = dynamic_cast<const MapLiteral *>(&expr)) {
    return CloneMapLiteral(*node);
  }
  if (const auto *node = dynamic_cast<const Variable *>(&expr)) {
    return CloneVariable(*node);
  }
  if (const auto *node = dynamic_cast<const Parameter *>(&expr)) {
    return CloneParameter(*node);
  }
  if (const auto *node = dynamic_cast<const PropertyExpression *>(&expr)) {
    auto cloned = std::make_unique<PropertyExpression>();
    cloned->object = CloneExpressionPtr(node->object);
    cloned->property_key = node->property_key;
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ListIndexExpression *>(&expr)) {
    auto cloned = std::make_unique<ListIndexExpression>();
    cloned->list = CloneExpressionPtr(node->list);
    cloned->index = CloneExpressionPtr(node->index);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ListSliceExpression *>(&expr)) {
    auto cloned = std::make_unique<ListSliceExpression>();
    cloned->list = CloneExpressionPtr(node->list);
    cloned->start_index = CloneExpressionPtr(node->start_index);
    cloned->end_index = CloneExpressionPtr(node->end_index);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const FunctionInvocation *>(&expr)) {
    auto cloned = std::make_unique<FunctionInvocation>();
    cloned->function_name = node->function_name;
    cloned->distinct = node->distinct;
    cloned->arguments = CloneList(node->arguments, CloneExpressionImpl);
    return cloned;
  }
  if (dynamic_cast<const CountStarExpression *>(&expr) != nullptr) {
    return std::make_unique<CountStarExpression>();
  }
  if (const auto *node = dynamic_cast<const CaseExpression *>(&expr)) {
    auto cloned = std::make_unique<CaseExpression>();
    cloned->test = CloneExpressionPtr(node->test);
    cloned->alternatives.reserve(node->alternatives.size());
    for (const auto &alt : node->alternatives) {
      cloned->alternatives.emplace_back(CloneExpressionPtr(alt.first),
                                        CloneExpressionPtr(alt.second));
    }
    cloned->else_expr = CloneExpressionPtr(node->else_expr);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ParenthesizedExpression *>(&expr)) {
    auto cloned = std::make_unique<ParenthesizedExpression>();
    cloned->expr = CloneExpressionPtr(node->expr);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ListComprehension *>(&expr)) {
    auto cloned = std::make_unique<ListComprehension>();
    cloned->variable = node->variable;
    cloned->list_expr = CloneExpressionPtr(node->list_expr);
    cloned->where_expr = CloneExpressionPtr(node->where_expr);
    cloned->eval_expr = CloneExpressionPtr(node->eval_expr);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const PatternComprehension *>(&expr)) {
    auto cloned = std::make_unique<PatternComprehension>();
    cloned->variable = node->variable;
    cloned->relationships_pattern =
        CloneRelationshipsPatternPtr(node->relationships_pattern);
    cloned->where_expr = CloneExpressionPtr(node->where_expr);
    cloned->eval_expr = CloneExpressionPtr(node->eval_expr);
    return cloned;
  }
  if (const auto *node =
          dynamic_cast<const PatternPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<PatternPredicateExpression>();
    cloned->relationships_pattern =
        CloneRelationshipsPatternPtr(node->relationships_pattern);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const AllQuantifier *>(&expr)) {
    return CloneQuantifier(*node);
  }
  if (const auto *node = dynamic_cast<const AnyQuantifier *>(&expr)) {
    return CloneQuantifier(*node);
  }
  if (const auto *node = dynamic_cast<const NoneQuantifier *>(&expr)) {
    return CloneQuantifier(*node);
  }
  if (const auto *node = dynamic_cast<const SingleQuantifier *>(&expr)) {
    return CloneQuantifier(*node);
  }
  if (const auto *node = dynamic_cast<const ExistentialSubquery *>(&expr)) {
    auto cloned = std::make_unique<ExistentialSubquery>();
    if (node->query) {
      cloned->query = CloneRegularQuery(*node->query);
    }
    if (node->pattern) {
      cloned->pattern = ClonePattern(*node->pattern);
    }
    cloned->where_expr = CloneExpressionPtr(node->where_expr);
    return cloned;
  }
  THROW(InternalError, "Unsupported Expression type for clone");
}

std::unique_ptr<ListLiteral> CloneListLiteral(const ListLiteral &list) {
  auto cloned = std::make_unique<ListLiteral>();
  cloned->elements = CloneList(list.elements, CloneExpressionImpl);
  return cloned;
}

std::unique_ptr<MapLiteral> CloneMapLiteral(const MapLiteral &map) {
  auto cloned = std::make_unique<MapLiteral>();
  cloned->entries.reserve(map.entries.size());
  for (const auto &entry : map.entries) {
    cloned->entries.emplace_back(entry.first, CloneExpressionPtr(entry.second));
  }
  return cloned;
}

std::unique_ptr<Properties> CloneProperties(const Properties &props) {
  auto cloned = std::make_unique<Properties>();
  cloned->map = CloneMaybe(props.map, CloneMapLiteral);
  cloned->parameter = CloneMaybe(props.parameter, CloneParameter);
  return cloned;
}

std::unique_ptr<NodePattern> CloneNodePattern(const NodePattern &node) {
  auto cloned = std::make_unique<NodePattern>();
  cloned->variable = node.variable;
  cloned->labels = node.labels;
  cloned->properties = ClonePropertiesPtr(node.properties);
  return cloned;
}

std::unique_ptr<RelationshipDetail> CloneRelationshipDetail(
    const RelationshipDetail &detail) {
  auto cloned = std::make_unique<RelationshipDetail>();
  cloned->variable = detail.variable;
  cloned->types = detail.types;
  cloned->range = detail.range;
  cloned->properties = ClonePropertiesPtr(detail.properties);
  return cloned;
}

std::unique_ptr<RelationshipPattern> CloneRelationshipPattern(
    const RelationshipPattern &pattern) {
  auto cloned = std::make_unique<RelationshipPattern>();
  cloned->left_arrow = pattern.left_arrow;
  cloned->right_arrow = pattern.right_arrow;
  cloned->detail = CloneMaybe(pattern.detail, CloneRelationshipDetail);
  return cloned;
}

std::unique_ptr<PatternElement> ClonePatternElement(
    const PatternElement &element) {
  auto cloned = std::make_unique<PatternElement>();
  cloned->node_pattern = CloneMaybe(element.node_pattern, CloneNodePattern);
  cloned->chain.reserve(element.chain.size());
  for (const auto &link : element.chain) {
    cloned->chain.emplace_back(CloneMaybe(link.first, CloneRelationshipPattern),
                               CloneMaybe(link.second, CloneNodePattern));
  }
  return cloned;
}

std::unique_ptr<PatternPart> ClonePatternPart(const PatternPart &part) {
  auto cloned = std::make_unique<PatternPart>();
  cloned->variable = part.variable;
  cloned->element = CloneMaybe(part.element, ClonePatternElement);
  return cloned;
}

std::unique_ptr<Pattern> ClonePattern(const Pattern &pattern) {
  auto cloned = std::make_unique<Pattern>();
  cloned->parts = CloneList(pattern.parts, ClonePatternPart);
  return cloned;
}

std::unique_ptr<RelationshipsPattern> CloneRelationshipsPattern(
    const RelationshipsPattern &pattern) {
  auto cloned = std::make_unique<RelationshipsPattern>();
  cloned->node_pattern = CloneMaybe(pattern.node_pattern, CloneNodePattern);
  cloned->chain.reserve(pattern.chain.size());
  for (const auto &link : pattern.chain) {
    cloned->chain.emplace_back(CloneMaybe(link.first, CloneRelationshipPattern),
                               CloneMaybe(link.second, CloneNodePattern));
  }
  return cloned;
}

std::unique_ptr<Match> CloneMatch(const Match &match) {
  auto cloned = std::make_unique<Match>();
  cloned->optional_match = match.optional_match;
  cloned->pattern = ClonePatternPtr(match.pattern);
  cloned->where = CloneExpressionPtr(match.where);
  return cloned;
}

std::unique_ptr<Unwind> CloneUnwind(const Unwind &unwind) {
  auto cloned = std::make_unique<Unwind>();
  cloned->expression = CloneExpressionPtr(unwind.expression);
  cloned->variable = unwind.variable;
  return cloned;
}

std::unique_ptr<InQueryCall> CloneInQueryCall(const InQueryCall &call) {
  auto cloned = std::make_unique<InQueryCall>();
  cloned->procedure_name = call.procedure_name;
  cloned->arguments = CloneList(call.arguments, CloneExpressionImpl);
  cloned->yield_items = call.yield_items;
  cloned->yield_where = CloneExpressionPtr(call.yield_where);
  return cloned;
}

std::unique_ptr<ReadingClause> CloneReadingClause(const ReadingClause &clause) {
  if (const auto *node = dynamic_cast<const Match *>(&clause)) {
    return CloneMatch(*node);
  }
  if (const auto *node = dynamic_cast<const Unwind *>(&clause)) {
    return CloneUnwind(*node);
  }
  if (const auto *node = dynamic_cast<const InQueryCall *>(&clause)) {
    return CloneInQueryCall(*node);
  }
  THROW(InternalError, "Unsupported ReadingClause type for clone");
}

std::unique_ptr<Create> CloneCreate(const Create &create) {
  auto cloned = std::make_unique<Create>();
  cloned->pattern = ClonePatternPtr(create.pattern);
  return cloned;
}

std::unique_ptr<SetItem> CloneSetItem(const SetItem &item) {
  auto cloned = std::make_unique<SetItem>();
  cloned->type = item.type;
  cloned->target = CloneExpressionPtr(item.target);
  cloned->value = CloneExpressionPtr(item.value);
  cloned->labels = item.labels;
  cloned->plus_equal = item.plus_equal;
  return cloned;
}

std::unique_ptr<Set> CloneSet(const Set &set) {
  auto cloned = std::make_unique<Set>();
  cloned->items = CloneList(set.items, CloneSetItem);
  return cloned;
}

std::unique_ptr<Merge> CloneMerge(const Merge &merge) {
  auto cloned = std::make_unique<Merge>();
  cloned->pattern_part = CloneMaybe(merge.pattern_part, ClonePatternPart);
  cloned->actions.reserve(merge.actions.size());
  for (const auto &action : merge.actions) {
    cloned->actions.emplace_back(action.first,
                                 CloneMaybe(action.second, CloneSet));
  }
  return cloned;
}

std::unique_ptr<Delete> CloneDelete(const Delete &del) {
  auto cloned = std::make_unique<Delete>();
  cloned->detach = del.detach;
  cloned->expressions = CloneList(del.expressions, CloneExpressionImpl);
  return cloned;
}

std::unique_ptr<RemoveItem> CloneRemoveItem(const RemoveItem &item) {
  auto cloned = std::make_unique<RemoveItem>();
  cloned->type = item.type;
  cloned->target = CloneExpressionPtr(item.target);
  cloned->labels = item.labels;
  return cloned;
}

std::unique_ptr<Remove> CloneRemove(const Remove &remove) {
  auto cloned = std::make_unique<Remove>();
  cloned->items = CloneList(remove.items, CloneRemoveItem);
  return cloned;
}

std::unique_ptr<UpdatingClause> CloneUpdatingClause(
    const UpdatingClause &clause) {
  if (const auto *node = dynamic_cast<const Create *>(&clause)) {
    return CloneCreate(*node);
  }
  if (const auto *node = dynamic_cast<const Merge *>(&clause)) {
    return CloneMerge(*node);
  }
  if (const auto *node = dynamic_cast<const Delete *>(&clause)) {
    return CloneDelete(*node);
  }
  if (const auto *node = dynamic_cast<const Set *>(&clause)) {
    return CloneSet(*node);
  }
  if (const auto *node = dynamic_cast<const Remove *>(&clause)) {
    return CloneRemove(*node);
  }
  THROW(InternalError, "Unsupported UpdatingClause type for clone");
}

std::unique_ptr<ProjectionItem> CloneProjectionItem(
    const ProjectionItem &item) {
  auto cloned = std::make_unique<ProjectionItem>();
  cloned->expression = CloneExpressionPtr(item.expression);
  cloned->alias = item.alias;
  return cloned;
}

std::unique_ptr<SortItem> CloneSortItem(const SortItem &item) {
  auto cloned = std::make_unique<SortItem>();
  cloned->expression = CloneExpressionPtr(item.expression);
  cloned->ascending = item.ascending;
  return cloned;
}

std::unique_ptr<ProjectionBody> CloneProjectionBody(
    const ProjectionBody &body) {
  auto cloned = std::make_unique<ProjectionBody>();
  cloned->distinct = body.distinct;
  cloned->star = body.star;
  cloned->items = CloneList(body.items, CloneProjectionItem);
  cloned->order_by = CloneList(body.order_by, CloneSortItem);
  cloned->skip = CloneExpressionPtr(body.skip);
  cloned->limit = CloneExpressionPtr(body.limit);
  return cloned;
}

std::unique_ptr<With> CloneWith(const With &with_clause) {
  auto cloned = std::make_unique<With>();
  cloned->body = CloneMaybe(with_clause.body, CloneProjectionBody);
  cloned->where = CloneExpressionPtr(with_clause.where);
  return cloned;
}

std::unique_ptr<Return> CloneReturn(const Return &return_clause) {
  auto cloned = std::make_unique<Return>();
  cloned->body = CloneMaybe(return_clause.body, CloneProjectionBody);
  return cloned;
}

std::unique_ptr<SinglePartQuery> CloneSinglePartQuery(
    const SinglePartQuery &query) {
  auto cloned = std::make_unique<SinglePartQuery>();
  cloned->reading_clauses =
      CloneList(query.reading_clauses, CloneReadingClause);
  cloned->updating_clauses =
      CloneList(query.updating_clauses, CloneUpdatingClause);
  cloned->return_clause = CloneMaybe(query.return_clause, CloneReturn);
  return cloned;
}

std::unique_ptr<MultiPartQuery> CloneMultiPartQuery(
    const MultiPartQuery &query) {
  auto cloned = std::make_unique<MultiPartQuery>();
  cloned->parts.reserve(query.parts.size());
  for (const auto &part : query.parts) {
    MultiPartQuery::WithPart cloned_part;
    cloned_part.reading_clauses =
        CloneList(part.reading_clauses, CloneReadingClause);
    cloned_part.updating_clauses =
        CloneList(part.updating_clauses, CloneUpdatingClause);
    cloned_part.with_clause = CloneMaybe(part.with_clause, CloneWith);
    cloned->parts.push_back(std::move(cloned_part));
  }
  cloned->final_single_part_query =
      CloneMaybe(query.final_single_part_query, CloneSinglePartQuery);
  return cloned;
}

std::unique_ptr<SingleQuery> CloneSingleQuery(const SingleQuery &query) {
  if (const auto *node = dynamic_cast<const SinglePartQuery *>(&query)) {
    return CloneSinglePartQuery(*node);
  }
  if (const auto *node = dynamic_cast<const MultiPartQuery *>(&query)) {
    return CloneMultiPartQuery(*node);
  }
  THROW(InternalError, "Unsupported SingleQuery type for clone");
}

std::unique_ptr<UnionPart> CloneUnionPart(const UnionPart &part) {
  auto cloned = std::make_unique<UnionPart>();
  cloned->all = part.all;
  cloned->query = CloneMaybe(part.query, CloneSingleQuery);
  return cloned;
}

std::unique_ptr<RegularQuery> CloneRegularQuery(const RegularQuery &query) {
  auto cloned = std::make_unique<RegularQuery>();
  cloned->single_query = CloneMaybe(query.single_query, CloneSingleQuery);
  cloned->unions = CloneList(query.unions, CloneUnionPart);
  return cloned;
}

}  // namespace

std::unique_ptr<Expression> cloneExpression(const Expression &expr) {
  return CloneExpressionImpl(expr);
}

}  // namespace ast
