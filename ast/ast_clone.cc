#include "ast_clone.h"

#include <utility>

#include "common/exception.h"

namespace ast {

using common::InternalError;

namespace {

template <typename T, typename CloneFunc>
std::unique_ptr<T> cloneMaybe(const std::unique_ptr<T> &ptr,
                              CloneFunc clone_func) {
  if (!ptr) {
    return nullptr;
  }
  return clone_func(*ptr);
}

template <typename T, typename CloneFunc>
std::vector<std::unique_ptr<T>> cloneList(
    const std::vector<std::unique_ptr<T>> &list, CloneFunc clone_func) {
  std::vector<std::unique_ptr<T>> cloned;
  cloned.reserve(list.size());
  for (const auto &item : list) {
    cloned.push_back(cloneMaybe(item, clone_func));
  }
  return cloned;
}

std::unique_ptr<Expression> cloneExpressionImpl(const Expression &expr);
std::unique_ptr<RegularQuery> cloneRegularQuery(const RegularQuery &query);
std::unique_ptr<SingleQuery> cloneSingleQuery(const SingleQuery &query);
std::unique_ptr<SinglePartQuery> cloneSinglePartQuery(
    const SinglePartQuery &query);
std::unique_ptr<MultiPartQuery> cloneMultiPartQuery(
    const MultiPartQuery &query);
std::unique_ptr<UnionPart> cloneUnionPart(const UnionPart &part);
std::unique_ptr<Pattern> clonePattern(const Pattern &pattern);
std::unique_ptr<PatternPart> clonePatternPart(const PatternPart &part);
std::unique_ptr<PatternElement> clonePatternElement(
    const PatternElement &element);
std::unique_ptr<RelationshipsPattern> cloneRelationshipsPattern(
    const RelationshipsPattern &pattern);
std::unique_ptr<NodePattern> cloneNodePattern(const NodePattern &node);
std::unique_ptr<RelationshipPattern> cloneRelationshipPattern(
    const RelationshipPattern &pattern);
std::unique_ptr<RelationshipDetail> cloneRelationshipDetail(
    const RelationshipDetail &detail);
std::unique_ptr<Properties> cloneProperties(const Properties &props);
std::unique_ptr<MapLiteral> cloneMapLiteral(const MapLiteral &map);
std::unique_ptr<ListLiteral> cloneListLiteral(const ListLiteral &list);
std::unique_ptr<ReadingClause> cloneReadingClause(const ReadingClause &clause);
std::unique_ptr<UpdatingClause> cloneUpdatingClause(
    const UpdatingClause &clause);
std::unique_ptr<Match> cloneMatch(const Match &match);
std::unique_ptr<Unwind> cloneUnwind(const Unwind &unwind);
std::unique_ptr<InQueryCall> cloneInQueryCall(const InQueryCall &call);
std::unique_ptr<Create> cloneCreate(const Create &create);
std::unique_ptr<Merge> cloneMerge(const Merge &merge);
std::unique_ptr<Delete> cloneDelete(const Delete &del);
std::unique_ptr<Set> cloneSet(const Set &set);
std::unique_ptr<SetItem> cloneSetItem(const SetItem &item);
std::unique_ptr<Remove> cloneRemove(const Remove &remove);
std::unique_ptr<RemoveItem> cloneRemoveItem(const RemoveItem &item);
std::unique_ptr<ProjectionBody> cloneProjectionBody(const ProjectionBody &body);
std::unique_ptr<ProjectionItem> cloneProjectionItem(const ProjectionItem &item);
std::unique_ptr<SortItem> cloneSortItem(const SortItem &item);
std::unique_ptr<With> cloneWith(const With &with_clause);
std::unique_ptr<Return> cloneReturn(const Return &return_clause);

std::unique_ptr<Expression> cloneExpressionPtr(
    const std::unique_ptr<Expression> &expr) {
  return cloneMaybe(expr, cloneExpressionImpl);
}

std::unique_ptr<Pattern> clonePatternPtr(
    const std::unique_ptr<Pattern> &pattern) {
  return cloneMaybe(pattern, clonePattern);
}

std::unique_ptr<RelationshipsPattern> cloneRelationshipsPatternPtr(
    const std::unique_ptr<RelationshipsPattern> &pattern) {
  return cloneMaybe(pattern, cloneRelationshipsPattern);
}

std::unique_ptr<Properties> clonePropertiesPtr(
    const std::unique_ptr<Properties> &props) {
  return cloneMaybe(props, cloneProperties);
}

std::unique_ptr<Variable> cloneVariable(const Variable &var) {
  auto cloned = std::make_unique<Variable>();
  cloned->name = var.name;
  return cloned;
}

std::unique_ptr<Parameter> cloneParameter(const Parameter &param) {
  auto cloned = std::make_unique<Parameter>();
  cloned->name = param.name;
  return cloned;
}

template <typename T>
std::unique_ptr<T> cloneBinary(const T &node) {
  auto cloned = std::make_unique<T>();
  cloned->left = cloneExpressionPtr(node.left);
  cloned->right = cloneExpressionPtr(node.right);
  return cloned;
}

template <typename T>
std::unique_ptr<T> cloneUnary(const T &node) {
  auto cloned = std::make_unique<T>();
  cloned->operand = cloneExpressionPtr(node.operand);
  return cloned;
}

template <typename T>
std::unique_ptr<T> cloneQuantifier(const T &node) {
  auto cloned = std::make_unique<T>();
  cloned->variable = node.variable;
  cloned->list_expr = cloneExpressionPtr(node.list_expr);
  cloned->predicate = cloneExpressionPtr(node.predicate);
  return cloned;
}

std::unique_ptr<Expression> cloneExpressionImpl(const Expression &expr) {
  if (const auto *node = dynamic_cast<const OrExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const XorExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const AndExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const ComparisonExpression *>(&expr)) {
    auto cloned = std::make_unique<ComparisonExpression>();
    cloned->left = cloneExpressionPtr(node->left);
    cloned->op = node->op;
    cloned->right = cloneExpressionPtr(node->right);
    return cloned;
  }
  if (const auto *node =
          dynamic_cast<const ComparisonChainExpression *>(&expr)) {
    auto cloned = std::make_unique<ComparisonChainExpression>();
    cloned->left = cloneExpressionPtr(node->left);
    cloned->rights.reserve(node->rights.size());
    for (const auto &entry : node->rights) {
      cloned->rights.emplace_back(entry.first,
                                  cloneExpressionPtr(entry.second));
    }
    return cloned;
  }
  if (const auto *node = dynamic_cast<const AddExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const SubtractExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const MultiplyExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const DivideExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const ModuloExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const PowerExpression *>(&expr)) {
    return cloneBinary(*node);
  }
  if (const auto *node = dynamic_cast<const NotExpression *>(&expr)) {
    return cloneUnary(*node);
  }
  if (const auto *node = dynamic_cast<const UnaryPlusExpression *>(&expr)) {
    return cloneUnary(*node);
  }
  if (const auto *node = dynamic_cast<const UnaryMinusExpression *>(&expr)) {
    return cloneUnary(*node);
  }
  if (const auto *node =
          dynamic_cast<const StringPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<StringPredicateExpression>();
    cloned->left = cloneExpressionPtr(node->left);
    cloned->op = node->op;
    cloned->right = cloneExpressionPtr(node->right);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ListPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<ListPredicateExpression>();
    cloned->element = cloneExpressionPtr(node->element);
    cloned->list = cloneExpressionPtr(node->list);
    return cloned;
  }
  if (const auto *node =
          dynamic_cast<const LabelPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<LabelPredicateExpression>();
    cloned->expr = cloneExpressionPtr(node->expr);
    cloned->labels = node->labels;
    return cloned;
  }
  if (const auto *node = dynamic_cast<const NullPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<NullPredicateExpression>();
    cloned->operand = cloneExpressionPtr(node->operand);
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
  if (dynamic_cast<const NullLiteral *>(&expr)) {
    return std::make_unique<NullLiteral>();
  }
  if (const auto *node = dynamic_cast<const ListLiteral *>(&expr)) {
    return cloneListLiteral(*node);
  }
  if (const auto *node = dynamic_cast<const MapLiteral *>(&expr)) {
    return cloneMapLiteral(*node);
  }
  if (const auto *node = dynamic_cast<const Variable *>(&expr)) {
    return cloneVariable(*node);
  }
  if (const auto *node = dynamic_cast<const Parameter *>(&expr)) {
    return cloneParameter(*node);
  }
  if (const auto *node = dynamic_cast<const PropertyExpression *>(&expr)) {
    auto cloned = std::make_unique<PropertyExpression>();
    cloned->object = cloneExpressionPtr(node->object);
    cloned->property_key = node->property_key;
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ListIndexExpression *>(&expr)) {
    auto cloned = std::make_unique<ListIndexExpression>();
    cloned->list = cloneExpressionPtr(node->list);
    cloned->index = cloneExpressionPtr(node->index);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ListSliceExpression *>(&expr)) {
    auto cloned = std::make_unique<ListSliceExpression>();
    cloned->list = cloneExpressionPtr(node->list);
    cloned->start_index = cloneExpressionPtr(node->start_index);
    cloned->end_index = cloneExpressionPtr(node->end_index);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const FunctionInvocation *>(&expr)) {
    auto cloned = std::make_unique<FunctionInvocation>();
    cloned->function_name = node->function_name;
    cloned->distinct = node->distinct;
    cloned->arguments = cloneList(node->arguments, cloneExpressionImpl);
    return cloned;
  }
  if (dynamic_cast<const CountStarExpression *>(&expr)) {
    return std::make_unique<CountStarExpression>();
  }
  if (const auto *node = dynamic_cast<const CaseExpression *>(&expr)) {
    auto cloned = std::make_unique<CaseExpression>();
    cloned->test = cloneExpressionPtr(node->test);
    cloned->alternatives.reserve(node->alternatives.size());
    for (const auto &alt : node->alternatives) {
      cloned->alternatives.emplace_back(cloneExpressionPtr(alt.first),
                                        cloneExpressionPtr(alt.second));
    }
    cloned->else_expr = cloneExpressionPtr(node->else_expr);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ParenthesizedExpression *>(&expr)) {
    auto cloned = std::make_unique<ParenthesizedExpression>();
    cloned->expr = cloneExpressionPtr(node->expr);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const ListComprehension *>(&expr)) {
    auto cloned = std::make_unique<ListComprehension>();
    cloned->variable = node->variable;
    cloned->list_expr = cloneExpressionPtr(node->list_expr);
    cloned->where_expr = cloneExpressionPtr(node->where_expr);
    cloned->eval_expr = cloneExpressionPtr(node->eval_expr);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const PatternComprehension *>(&expr)) {
    auto cloned = std::make_unique<PatternComprehension>();
    cloned->variable = node->variable;
    cloned->relationships_pattern =
        cloneRelationshipsPatternPtr(node->relationships_pattern);
    cloned->where_expr = cloneExpressionPtr(node->where_expr);
    cloned->eval_expr = cloneExpressionPtr(node->eval_expr);
    return cloned;
  }
  if (const auto *node =
          dynamic_cast<const PatternPredicateExpression *>(&expr)) {
    auto cloned = std::make_unique<PatternPredicateExpression>();
    cloned->relationships_pattern =
        cloneRelationshipsPatternPtr(node->relationships_pattern);
    return cloned;
  }
  if (const auto *node = dynamic_cast<const AllQuantifier *>(&expr)) {
    return cloneQuantifier(*node);
  }
  if (const auto *node = dynamic_cast<const AnyQuantifier *>(&expr)) {
    return cloneQuantifier(*node);
  }
  if (const auto *node = dynamic_cast<const NoneQuantifier *>(&expr)) {
    return cloneQuantifier(*node);
  }
  if (const auto *node = dynamic_cast<const SingleQuantifier *>(&expr)) {
    return cloneQuantifier(*node);
  }
  if (const auto *node = dynamic_cast<const ExistentialSubquery *>(&expr)) {
    auto cloned = std::make_unique<ExistentialSubquery>();
    if (node->query) {
      cloned->query = cloneRegularQuery(*node->query);
    }
    if (node->pattern) {
      cloned->pattern = clonePattern(*node->pattern);
    }
    cloned->where_expr = cloneExpressionPtr(node->where_expr);
    return cloned;
  }
  THROW(InternalError, "Unsupported Expression type for clone");
}

std::unique_ptr<ListLiteral> cloneListLiteral(const ListLiteral &list) {
  auto cloned = std::make_unique<ListLiteral>();
  cloned->elements = cloneList(list.elements, cloneExpressionImpl);
  return cloned;
}

std::unique_ptr<MapLiteral> cloneMapLiteral(const MapLiteral &map) {
  auto cloned = std::make_unique<MapLiteral>();
  cloned->entries.reserve(map.entries.size());
  for (const auto &entry : map.entries) {
    cloned->entries.emplace_back(entry.first, cloneExpressionPtr(entry.second));
  }
  return cloned;
}

std::unique_ptr<Properties> cloneProperties(const Properties &props) {
  auto cloned = std::make_unique<Properties>();
  cloned->map = cloneMaybe(props.map, cloneMapLiteral);
  cloned->parameter = cloneMaybe(props.parameter, cloneParameter);
  return cloned;
}

std::unique_ptr<NodePattern> cloneNodePattern(const NodePattern &node) {
  auto cloned = std::make_unique<NodePattern>();
  cloned->variable = node.variable;
  cloned->labels = node.labels;
  cloned->properties = clonePropertiesPtr(node.properties);
  return cloned;
}

std::unique_ptr<RelationshipDetail> cloneRelationshipDetail(
    const RelationshipDetail &detail) {
  auto cloned = std::make_unique<RelationshipDetail>();
  cloned->variable = detail.variable;
  cloned->types = detail.types;
  cloned->range = detail.range;
  cloned->properties = clonePropertiesPtr(detail.properties);
  return cloned;
}

std::unique_ptr<RelationshipPattern> cloneRelationshipPattern(
    const RelationshipPattern &pattern) {
  auto cloned = std::make_unique<RelationshipPattern>();
  cloned->left_arrow = pattern.left_arrow;
  cloned->right_arrow = pattern.right_arrow;
  cloned->detail = cloneMaybe(pattern.detail, cloneRelationshipDetail);
  return cloned;
}

std::unique_ptr<PatternElement> clonePatternElement(
    const PatternElement &element) {
  auto cloned = std::make_unique<PatternElement>();
  cloned->node_pattern = cloneMaybe(element.node_pattern, cloneNodePattern);
  cloned->chain.reserve(element.chain.size());
  for (const auto &link : element.chain) {
    cloned->chain.emplace_back(cloneMaybe(link.first, cloneRelationshipPattern),
                               cloneMaybe(link.second, cloneNodePattern));
  }
  return cloned;
}

std::unique_ptr<PatternPart> clonePatternPart(const PatternPart &part) {
  auto cloned = std::make_unique<PatternPart>();
  cloned->variable = part.variable;
  cloned->element = cloneMaybe(part.element, clonePatternElement);
  return cloned;
}

std::unique_ptr<Pattern> clonePattern(const Pattern &pattern) {
  auto cloned = std::make_unique<Pattern>();
  cloned->parts = cloneList(pattern.parts, clonePatternPart);
  return cloned;
}

std::unique_ptr<RelationshipsPattern> cloneRelationshipsPattern(
    const RelationshipsPattern &pattern) {
  auto cloned = std::make_unique<RelationshipsPattern>();
  cloned->node_pattern = cloneMaybe(pattern.node_pattern, cloneNodePattern);
  cloned->chain.reserve(pattern.chain.size());
  for (const auto &link : pattern.chain) {
    cloned->chain.emplace_back(cloneMaybe(link.first, cloneRelationshipPattern),
                               cloneMaybe(link.second, cloneNodePattern));
  }
  return cloned;
}

std::unique_ptr<Match> cloneMatch(const Match &match) {
  auto cloned = std::make_unique<Match>();
  cloned->optional_match = match.optional_match;
  cloned->pattern = clonePatternPtr(match.pattern);
  cloned->where = cloneExpressionPtr(match.where);
  return cloned;
}

std::unique_ptr<Unwind> cloneUnwind(const Unwind &unwind) {
  auto cloned = std::make_unique<Unwind>();
  cloned->expression = cloneExpressionPtr(unwind.expression);
  cloned->variable = unwind.variable;
  return cloned;
}

std::unique_ptr<InQueryCall> cloneInQueryCall(const InQueryCall &call) {
  auto cloned = std::make_unique<InQueryCall>();
  cloned->procedure_name = call.procedure_name;
  cloned->arguments = cloneList(call.arguments, cloneExpressionImpl);
  cloned->yield_items = call.yield_items;
  cloned->yield_where = cloneExpressionPtr(call.yield_where);
  return cloned;
}

std::unique_ptr<ReadingClause> cloneReadingClause(const ReadingClause &clause) {
  if (const auto *node = dynamic_cast<const Match *>(&clause)) {
    return cloneMatch(*node);
  }
  if (const auto *node = dynamic_cast<const Unwind *>(&clause)) {
    return cloneUnwind(*node);
  }
  if (const auto *node = dynamic_cast<const InQueryCall *>(&clause)) {
    return cloneInQueryCall(*node);
  }
  THROW(InternalError, "Unsupported ReadingClause type for clone");
}

std::unique_ptr<Create> cloneCreate(const Create &create) {
  auto cloned = std::make_unique<Create>();
  cloned->pattern = clonePatternPtr(create.pattern);
  return cloned;
}

std::unique_ptr<SetItem> cloneSetItem(const SetItem &item) {
  auto cloned = std::make_unique<SetItem>();
  cloned->type = item.type;
  cloned->target = cloneExpressionPtr(item.target);
  cloned->value = cloneExpressionPtr(item.value);
  cloned->labels = item.labels;
  cloned->plus_equal = item.plus_equal;
  return cloned;
}

std::unique_ptr<Set> cloneSet(const Set &set) {
  auto cloned = std::make_unique<Set>();
  cloned->items = cloneList(set.items, cloneSetItem);
  return cloned;
}

std::unique_ptr<Merge> cloneMerge(const Merge &merge) {
  auto cloned = std::make_unique<Merge>();
  cloned->pattern_part = cloneMaybe(merge.pattern_part, clonePatternPart);
  cloned->actions.reserve(merge.actions.size());
  for (const auto &action : merge.actions) {
    cloned->actions.emplace_back(action.first,
                                 cloneMaybe(action.second, cloneSet));
  }
  return cloned;
}

std::unique_ptr<Delete> cloneDelete(const Delete &del) {
  auto cloned = std::make_unique<Delete>();
  cloned->detach = del.detach;
  cloned->expressions = cloneList(del.expressions, cloneExpressionImpl);
  return cloned;
}

std::unique_ptr<RemoveItem> cloneRemoveItem(const RemoveItem &item) {
  auto cloned = std::make_unique<RemoveItem>();
  cloned->type = item.type;
  cloned->target = cloneExpressionPtr(item.target);
  cloned->labels = item.labels;
  return cloned;
}

std::unique_ptr<Remove> cloneRemove(const Remove &remove) {
  auto cloned = std::make_unique<Remove>();
  cloned->items = cloneList(remove.items, cloneRemoveItem);
  return cloned;
}

std::unique_ptr<UpdatingClause> cloneUpdatingClause(
    const UpdatingClause &clause) {
  if (const auto *node = dynamic_cast<const Create *>(&clause)) {
    return cloneCreate(*node);
  }
  if (const auto *node = dynamic_cast<const Merge *>(&clause)) {
    return cloneMerge(*node);
  }
  if (const auto *node = dynamic_cast<const Delete *>(&clause)) {
    return cloneDelete(*node);
  }
  if (const auto *node = dynamic_cast<const Set *>(&clause)) {
    return cloneSet(*node);
  }
  if (const auto *node = dynamic_cast<const Remove *>(&clause)) {
    return cloneRemove(*node);
  }
  THROW(InternalError, "Unsupported UpdatingClause type for clone");
}

std::unique_ptr<ProjectionItem> cloneProjectionItem(
    const ProjectionItem &item) {
  auto cloned = std::make_unique<ProjectionItem>();
  cloned->expression = cloneExpressionPtr(item.expression);
  cloned->alias = item.alias;
  return cloned;
}

std::unique_ptr<SortItem> cloneSortItem(const SortItem &item) {
  auto cloned = std::make_unique<SortItem>();
  cloned->expression = cloneExpressionPtr(item.expression);
  cloned->ascending = item.ascending;
  return cloned;
}

std::unique_ptr<ProjectionBody> cloneProjectionBody(
    const ProjectionBody &body) {
  auto cloned = std::make_unique<ProjectionBody>();
  cloned->distinct = body.distinct;
  cloned->star = body.star;
  cloned->items = cloneList(body.items, cloneProjectionItem);
  cloned->order_by = cloneList(body.order_by, cloneSortItem);
  cloned->skip = cloneExpressionPtr(body.skip);
  cloned->limit = cloneExpressionPtr(body.limit);
  return cloned;
}

std::unique_ptr<With> cloneWith(const With &with_clause) {
  auto cloned = std::make_unique<With>();
  cloned->body = cloneMaybe(with_clause.body, cloneProjectionBody);
  cloned->where = cloneExpressionPtr(with_clause.where);
  return cloned;
}

std::unique_ptr<Return> cloneReturn(const Return &return_clause) {
  auto cloned = std::make_unique<Return>();
  cloned->body = cloneMaybe(return_clause.body, cloneProjectionBody);
  return cloned;
}

std::unique_ptr<SinglePartQuery> cloneSinglePartQuery(
    const SinglePartQuery &query) {
  auto cloned = std::make_unique<SinglePartQuery>();
  cloned->reading_clauses =
      cloneList(query.reading_clauses, cloneReadingClause);
  cloned->updating_clauses =
      cloneList(query.updating_clauses, cloneUpdatingClause);
  cloned->return_clause = cloneMaybe(query.return_clause, cloneReturn);
  return cloned;
}

std::unique_ptr<MultiPartQuery> cloneMultiPartQuery(
    const MultiPartQuery &query) {
  auto cloned = std::make_unique<MultiPartQuery>();
  cloned->parts.reserve(query.parts.size());
  for (const auto &part : query.parts) {
    MultiPartQuery::WithPart cloned_part;
    cloned_part.reading_clauses =
        cloneList(part.reading_clauses, cloneReadingClause);
    cloned_part.updating_clauses =
        cloneList(part.updating_clauses, cloneUpdatingClause);
    cloned_part.with_clause = cloneMaybe(part.with_clause, cloneWith);
    cloned->parts.push_back(std::move(cloned_part));
  }
  cloned->final_single_part_query =
      cloneMaybe(query.final_single_part_query, cloneSinglePartQuery);
  return cloned;
}

std::unique_ptr<SingleQuery> cloneSingleQuery(const SingleQuery &query) {
  if (const auto *node = dynamic_cast<const SinglePartQuery *>(&query)) {
    return cloneSinglePartQuery(*node);
  }
  if (const auto *node = dynamic_cast<const MultiPartQuery *>(&query)) {
    return cloneMultiPartQuery(*node);
  }
  THROW(InternalError, "Unsupported SingleQuery type for clone");
}

std::unique_ptr<UnionPart> cloneUnionPart(const UnionPart &part) {
  auto cloned = std::make_unique<UnionPart>();
  cloned->all = part.all;
  cloned->query = cloneMaybe(part.query, cloneSingleQuery);
  return cloned;
}

std::unique_ptr<RegularQuery> cloneRegularQuery(const RegularQuery &query) {
  auto cloned = std::make_unique<RegularQuery>();
  cloned->single_query = cloneMaybe(query.single_query, cloneSingleQuery);
  cloned->unions = cloneList(query.unions, cloneUnionPart);
  return cloned;
}

}  // namespace

std::unique_ptr<Expression> cloneExpression(const Expression &expr) {
  return cloneExpressionImpl(expr);
}

}  // namespace ast
