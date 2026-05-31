#include "ir/planner_query_graph_builder.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_clone.h"
#include "ast/semantic_table.h"
#include "common/exception.h"
#include "ir/planner_query_internal.h"

namespace ir {
namespace {

class PatternConverter {
 public:
  explicit PatternConverter(QueryGraph *graph) : graph_(graph) {
    CHECK(graph_ != nullptr, common::InternalError,
          "pattern converter query graph is null");
  }

  void AddPattern(const ast::Pattern &pattern) {
    CHECK(!pattern.parts.empty(), common::InvalidArgumentError,
          Missing("pattern part"));
    for (const auto &part : pattern.parts) {
      CHECK(part != nullptr, common::InvalidArgumentError,
            Missing("pattern part"));
      AddPatternPart(*part);
    }
  }

  void AddRelationshipsPattern(const ast::RelationshipsPattern &pattern) {
    CHECK(pattern.node_pattern != nullptr, common::InvalidArgumentError,
          Missing("relationships pattern node"));
    CHECK(!pattern.chain.empty(), common::InvalidArgumentError,
          Missing("relationships pattern chain"));
    std::string left = AddNode(*pattern.node_pattern);
    for (const auto &link : pattern.chain) {
      CHECK(link.first != nullptr, common::InvalidArgumentError,
            Missing("relationship pattern"));
      CHECK(link.second != nullptr, common::InvalidArgumentError,
            Missing("node pattern"));
      std::string right = AddNode(*link.second);
      AddRelationship(*link.first, left, right);
      left = right;
    }
  }

 private:
  void AddPatternPart(const ast::PatternPart &part) {
    CHECK(part.element != nullptr, common::InvalidArgumentError,
          Missing("pattern element"));
    PathPattern path = AddPatternElement(*part.element);
    if (!part.variable.empty()) {
      graph_->pattern_paths.insert(part.variable);
      path.variable = part.variable;
      graph_->path_patterns.push_back(std::move(path));
    }
  }

  PathPattern AddPatternElement(const ast::PatternElement &element) {
    if (!element.node_pattern) {
      THROW(common::InternalError, "node_pattern is null");
    }
    PathPattern path;
    std::string left = AddNode(*element.node_pattern);
    path.nodes.push_back(left);
    for (const auto &link : element.chain) {
      CHECK(link.first != nullptr, common::InvalidArgumentError,
            Missing("relationship pattern"));
      CHECK(link.second != nullptr, common::InvalidArgumentError,
            Missing("node pattern"));
      std::string right = AddNode(*link.second);
      path.relationships.push_back(AddRelationship(*link.first, left, right));
      path.nodes.push_back(right);
      left = right;
    }
    return path;
  }

  std::string AddNode(const ast::NodePattern &node) {
    CHECK(!node.variable.empty(), common::InvalidArgumentError,
          Unsupported("anonymous node"));
    CHECK(node.labels.empty(), common::InvalidArgumentError,
          Unsupported("node with labels"));
    CHECK(!node.properties, common::InvalidArgumentError,
          Unsupported("node with properties"));
    graph_->pattern_nodes.insert(node.variable);
    return node.variable;
  }

  std::string AddRelationship(const ast::RelationshipPattern &pattern,
                              const std::string &left,
                              const std::string &right) {
    const ast::RelationshipDetail *detail = pattern.detail.get();
    CHECK(detail && !detail->variable.empty(), common::InvalidArgumentError,
          Unsupported("anonymous relationship"));
    CHECK(!detail->properties, common::InvalidArgumentError,
          Unsupported("relationship with properties"));

    PatternRelationship relationship;
    relationship.variable = detail->variable;
    relationship.left_node = left;
    relationship.right_node = right;
    relationship.types = detail->types;
    if (detail->range) {
      relationship.length.variable = true;
      relationship.length.min = detail->range->min;
      relationship.length.max = detail->range->max;
    }
    if (pattern.left_arrow) {
      relationship.direction = Direction::kIncoming;
    } else if (pattern.right_arrow) {
      relationship.direction = Direction::kOutgoing;
    } else {
      relationship.direction = Direction::kBoth;
    }
    std::string variable = relationship.variable;
    graph_->pattern_relationships.push_back(std::move(relationship));
    return variable;
  }

  QueryGraph *graph_ = nullptr;
};

Direction PatternDirection(const ast::RelationshipPattern &pattern) {
  if (pattern.left_arrow) {
    return Direction::kIncoming;
  }
  if (pattern.right_arrow) {
    return Direction::kOutgoing;
  }
  return Direction::kBoth;
}

class CreatePatternConverter {
 public:
  explicit CreatePatternConverter(
      std::unordered_set<LogicalVariable> existing_nodes = {})
      : existing_nodes_(std::move(existing_nodes)) {}

  CreatePattern Convert(const ast::Pattern &pattern) {
    CHECK(!pattern.parts.empty(), common::InvalidArgumentError,
          Missing("updating pattern part"));
    for (const auto &part : pattern.parts) {
      CHECK(part != nullptr, common::InvalidArgumentError,
            Missing("updating pattern part"));
      AddPatternPart(*part);
    }
    return std::move(pattern_);
  }

  CreatePattern Convert(const ast::PatternPart &part) {
    AddPatternPart(part);
    return std::move(pattern_);
  }

 private:
  void AddPatternPart(const ast::PatternPart &part) {
    CHECK(part.element != nullptr, common::InvalidArgumentError,
          Missing("updating pattern element"));
    PathPattern path = AddPatternElement(*part.element);
    if (!part.variable.empty()) {
      pattern_.path_variables.insert(part.variable);
      path.variable = part.variable;
      pattern_.path_patterns.push_back(std::move(path));
    }
  }

  PathPattern AddPatternElement(const ast::PatternElement &element) {
    CHECK(element.node_pattern != nullptr, common::InvalidArgumentError,
          Missing("updating node pattern"));
    PathPattern path;
    std::string left = AddNode(*element.node_pattern);
    path.nodes.push_back(left);
    for (const auto &link : element.chain) {
      CHECK(link.first != nullptr, common::InvalidArgumentError,
            Missing("updating relationship pattern"));
      CHECK(link.second != nullptr, common::InvalidArgumentError,
            Missing("updating node pattern"));
      std::string right = AddNode(*link.second);
      path.relationships.push_back(AddRelationship(*link.first, left, right));
      path.nodes.push_back(right);
      left = std::move(right);
    }
    return path;
  }

  std::string AddNode(const ast::NodePattern &node) {
    CHECK(!node.variable.empty(), common::InvalidArgumentError,
          Unsupported("anonymous node in updating pattern"));
    if (node_indexes_.contains(node.variable)) {
      CHECK(node.labels.empty() && !node.properties,
            common::InvalidArgumentError,
            Unsupported("reused updating node with labels or properties"));
      return node.variable;
    }

    CreateNodePattern create_node;
    create_node.variable = node.variable;
    create_node.labels = node.labels;
    create_node.properties = BuildPropertyMap(node.properties.get());
    create_node.previously_bound = existing_nodes_.contains(node.variable);
    if (create_node.previously_bound) {
      CHECK(create_node.labels.empty() && create_node.properties.empty(),
            common::InvalidArgumentError,
            Unsupported("bound updating node with labels or properties"));
    }

    const std::size_t index = pattern_.nodes.size();
    pattern_.nodes.push_back(std::move(create_node));
    node_indexes_.emplace(node.variable, index);
    if (!pattern_.nodes.back().previously_bound) {
      pattern_.commands.push_back(
          {.kind = CreateEntityKind::kNode, .index = index});
    }
    return node.variable;
  }

  std::string AddRelationship(const ast::RelationshipPattern &pattern,
                              const std::string &left,
                              const std::string &right) {
    const ast::RelationshipDetail *detail = pattern.detail.get();
    CHECK(detail != nullptr && !detail->variable.empty(),
          common::InvalidArgumentError,
          Unsupported("anonymous relationship in updating pattern"));
    CHECK(!detail->range.has_value(), common::InvalidArgumentError,
          Unsupported("variable-length relationship in updating pattern"));

    CreateRelationshipPattern create_relationship;
    create_relationship.variable = detail->variable;
    create_relationship.left_node = left;
    create_relationship.right_node = right;
    create_relationship.direction = PatternDirection(pattern);
    create_relationship.types = detail->types;
    create_relationship.properties = BuildPropertyMap(detail->properties.get());

    const std::size_t index = pattern_.relationships.size();
    std::string variable = create_relationship.variable;
    pattern_.relationships.push_back(std::move(create_relationship));
    pattern_.commands.push_back(
        {.kind = CreateEntityKind::kRelationship, .index = index});
    return variable;
  }

  CreatePattern pattern_;
  std::unordered_map<std::string, std::size_t> node_indexes_;
  std::unordered_set<LogicalVariable> existing_nodes_;
};

CreatePattern BuildCreatePattern(
    const ast::Pattern &pattern,
    std::unordered_set<LogicalVariable> existing_nodes = {}) {
  CreatePatternConverter converter(std::move(existing_nodes));
  return converter.Convert(pattern);
}

CreatePattern BuildCreatePattern(
    const ast::PatternPart &part,
    std::unordered_set<LogicalVariable> existing_nodes = {}) {
  CreatePatternConverter converter(std::move(existing_nodes));
  return converter.Convert(part);
}

PatternRelationship ToPatternRelationship(
    const CreateRelationshipPattern &relationship) {
  PatternRelationship out;
  out.variable = relationship.variable;
  out.left_node = relationship.left_node;
  out.right_node = relationship.right_node;
  out.direction = relationship.direction;
  out.types = relationship.types;
  return out;
}

std::unique_ptr<ast::Expression> MakeVariableExpression(
    const std::string &name) {
  auto variable = std::make_unique<ast::Variable>();
  variable->name = name;
  return variable;
}

std::unique_ptr<ast::Expression> MakePropertyExpression(
    const std::string &variable, const std::string &property_key) {
  auto property = std::make_unique<ast::PropertyExpression>();
  property->object = MakeVariableExpression(variable);
  property->property_key = property_key;
  return property;
}

const ast::Expression *AddMergeMatchExpression(
    MergeMatchGraph *match_graph, std::unique_ptr<ast::Expression> expression) {
  CHECK(match_graph != nullptr, common::InternalError,
        "merge match graph is null");
  CHECK(expression != nullptr, common::InternalError,
        "merge match expression is null");
  const ast::Expression *raw = expression.get();
  std::shared_ptr<ast::Expression> owned(std::move(expression));
  match_graph->predicate_expressions.push_back(std::move(owned));
  return raw;
}

const ast::Expression *BuildMergeMatchLabelExpression(
    MergeMatchGraph *match_graph, const PatternLabelPredicate &label) {
  CHECK(match_graph != nullptr, common::InternalError,
        "merge match graph is null");
  if (label.variable.empty() || label.labels.empty()) {
    return nullptr;
  }

  auto expression = std::make_unique<ast::LabelPredicateExpression>();
  expression->expr = MakeVariableExpression(label.variable);
  expression->labels = label.labels;
  return AddMergeMatchExpression(match_graph, std::move(expression));
}

const ast::Expression *BuildMergeMatchPropertyExpression(
    MergeMatchGraph *match_graph, const PatternPropertyEquality &equality) {
  CHECK(match_graph != nullptr, common::InternalError,
        "merge match graph is null");
  CHECK(equality.value != nullptr, common::InvalidArgumentError,
        "MERGE property equality value is null");
  if (equality.variable.empty() || equality.property_key.empty()) {
    return nullptr;
  }

  auto comparison = std::make_unique<ast::ComparisonExpression>();
  comparison->left =
      MakePropertyExpression(equality.variable, equality.property_key);
  comparison->op = "=";
  comparison->right = ast::CloneExpression(*equality.value);
  return AddMergeMatchExpression(match_graph, std::move(comparison));
}

MergeMatchGraph BuildMergeMatchGraph(
    const CreatePattern &pattern,
    std::unordered_set<LogicalVariable> argument_ids) {
  MergeMatchGraph match_graph;
  match_graph.argument_ids = std::move(argument_ids);
  for (const auto &node : pattern.nodes) {
    AddSymbol(&match_graph.pattern_nodes, node.variable);
    if (!node.labels.empty()) {
      PatternLabelPredicate label{.variable = node.variable,
                                  .labels = node.labels};
      label.expression = BuildMergeMatchLabelExpression(&match_graph, label);
      match_graph.node_labels.push_back(std::move(label));
    }
    for (const auto &entry : node.properties.entries) {
      PatternPropertyEquality equality{.variable = node.variable,
                                       .property_key = entry.key,
                                       .value = entry.value};
      equality.expression =
          BuildMergeMatchPropertyExpression(&match_graph, equality);
      match_graph.property_equalities.push_back(std::move(equality));
    }
  }
  for (const auto &relationship : pattern.relationships) {
    AddSymbol(&match_graph.pattern_nodes, relationship.left_node);
    AddSymbol(&match_graph.pattern_nodes, relationship.right_node);
    match_graph.pattern_relationships.push_back(
        ToPatternRelationship(relationship));
    for (const auto &entry : relationship.properties.entries) {
      PatternPropertyEquality equality{.variable = relationship.variable,
                                       .property_key = entry.key,
                                       .value = entry.value};
      equality.expression =
          BuildMergeMatchPropertyExpression(&match_graph, equality);
      match_graph.property_equalities.push_back(std::move(equality));
    }
  }
  return match_graph;
}

SetMutatingPattern BuildSetMutatingPattern(const ast::SetItem &item) {
  SetMutatingPattern pattern;
  switch (item.type) {
    case ast::SetItem::Type::kProperty: {
      const ast::PropertyExpression *property =
          AsPropertyExpression(item.target.get());
      CHECK(property != nullptr, common::InvalidArgumentError,
            Missing("SET property target"));
      pattern.kind = SetMutatingPatternKind::kSetProperty;
      pattern.entity = property->object.get();
      pattern.property_key = property->property_key;
      pattern.value = item.value.get();
      return pattern;
    }
    case ast::SetItem::Type::kVariable:
      pattern.kind =
          item.plus_equal
              ? SetMutatingPatternKind::kSetIncludingPropertiesFromMap
              : SetMutatingPatternKind::kSetExactPropertiesFromMap;
      pattern.entity = item.target.get();
      pattern.value = item.value.get();
      return pattern;
    case ast::SetItem::Type::kLabels:
      pattern.kind = SetMutatingPatternKind::kSetLabels;
      pattern.entity = item.target.get();
      pattern.labels = item.labels;
      return pattern;
  }
  THROW(common::InternalError, "unknown SET item kind");
}

std::vector<SetMutatingPattern> BuildSetMutatingPatterns(const ast::Set &set) {
  std::vector<SetMutatingPattern> patterns;
  patterns.reserve(set.items.size());
  for (const auto &item : set.items) {
    CHECK(item != nullptr, common::InvalidArgumentError, Missing("SET item"));
    patterns.push_back(BuildSetMutatingPattern(*item));
  }
  return patterns;
}

RemoveMutatingPattern BuildRemoveMutatingPattern(const ast::RemoveItem &item) {
  RemoveMutatingPattern pattern;
  switch (item.type) {
    case ast::RemoveItem::Type::kProperty: {
      const ast::PropertyExpression *property =
          AsPropertyExpression(item.target.get());
      CHECK(property != nullptr, common::InvalidArgumentError,
            Missing("REMOVE property target"));
      pattern.kind = RemoveMutatingPatternKind::kRemoveProperty;
      pattern.entity = property->object.get();
      pattern.property_key = property->property_key;
      return pattern;
    }
    case ast::RemoveItem::Type::kLabels:
      pattern.kind = RemoveMutatingPatternKind::kRemoveLabels;
      pattern.entity = item.target.get();
      pattern.labels = item.labels;
      return pattern;
  }
  THROW(common::InternalError, "unknown REMOVE item kind");
}

std::vector<RemoveMutatingPattern> BuildRemoveMutatingPatterns(
    const ast::Remove &remove) {
  std::vector<RemoveMutatingPattern> patterns;
  patterns.reserve(remove.items.size());
  for (const auto &item : remove.items) {
    CHECK(item != nullptr, common::InvalidArgumentError,
          Missing("REMOVE item"));
    patterns.push_back(BuildRemoveMutatingPattern(*item));
  }
  return patterns;
}

MergePattern BuildMergePattern(
    const ast::Merge &merge,
    const std::unordered_set<std::string> &available_arguments,
    std::unordered_set<LogicalVariable> existing_nodes) {
  CHECK(merge.pattern_part != nullptr, common::InvalidArgumentError,
        Missing("MERGE pattern"));
  MergePattern pattern;
  pattern.create_pattern =
      BuildCreatePattern(*merge.pattern_part, std::move(existing_nodes));
  pattern.actions.reserve(merge.actions.size());
  for (const auto &action : merge.actions) {
    CHECK(action.second != nullptr, common::InvalidArgumentError,
          Missing("MERGE action SET"));
    MergeActionPattern action_pattern;
    action_pattern.on_match = action.first;
    action_pattern.set_patterns = BuildSetMutatingPatterns(*action.second);
    pattern.actions.push_back(std::move(action_pattern));
  }

  std::unordered_set<std::string> dependencies;
  AddCreatePatternDependencySymbols(&dependencies, pattern.create_pattern);
  for (const auto &action : pattern.actions) {
    AddSetMutatingPatternDependencySymbols(&dependencies, action.set_patterns);
  }
  pattern.match_graph =
      BuildMergeMatchGraph(pattern.create_pattern,
                           IntersectSymbols(dependencies, available_arguments));
  return pattern;
}

}  // namespace

void AddPatternToQueryGraph(QueryGraph *query_graph,
                            const ast::Pattern &pattern) {
  PatternConverter converter(query_graph);
  converter.AddPattern(pattern);
}

void AddRelationshipsPatternToQueryGraph(
    QueryGraph *query_graph, const ast::RelationshipsPattern &pattern) {
  PatternConverter converter(query_graph);
  converter.AddRelationshipsPattern(pattern);
}

QueryGraphBuilder::QueryGraphBuilder(
    const ast::SemanticTable &semantic_table,
    std::unordered_set<std::string> argument_ids) {
  semantic_table_ = &semantic_table;
  graph_.argument_ids = std::move(argument_ids);
}

void QueryGraphBuilder::BuildReadingClause(const ast::ReadingClause &clause) {
  switch (clause.node_type) {
    case ast::ASTNodeType::kMatch:
      BuildMatch(ast::CastAst<ast::Match>(clause));
      return;
    case ast::ASTNodeType::kUnwind:
      THROW(common::InvalidArgumentError, Unsupported("UNWIND"));
    case ast::ASTNodeType::kInQueryCall:
      THROW(common::InvalidArgumentError, Unsupported("procedure call"));
    default:
      break;
  }
  THROW(common::InvalidArgumentError, Unsupported("reading clause"));
}

void QueryGraphBuilder::BuildUpdatingClause(const ast::UpdatingClause &clause) {
  graph_.mutating_patterns.push_back(BuildMutatingPattern(clause));
}

bool QueryGraphBuilder::HasLocalWork() const { return graph_.HasLocalWork(); }

bool QueryGraphBuilder::HasOptionalMatches() const {
  return !graph_.optional_matches.empty();
}

bool QueryGraphBuilder::ContainsUpdates() const {
  return graph_.ContainsUpdates();
}

std::unordered_set<LogicalVariable> QueryGraphBuilder::AvailableSymbols()
    const {
  return QueryGraphAvailableSymbols(graph_);
}

void QueryGraphBuilder::AddWhere(const ast::Expression *where) {
  AddSelectionPredicates(where, SemanticTableRef(), &graph_, &graph_.selections,
                         &where_keys_);
}

QueryGraph QueryGraphBuilder::Release() { return std::move(graph_); }

void QueryGraphBuilder::BuildMatch(const ast::Match &match) {
  if (match.optional_match) {
    BuildOptionalMatch(match);
    return;
  }
  BuildRequiredMatch(match);
}

void QueryGraphBuilder::BuildOptionalMatch(const ast::Match &match) {
  QueryGraphBuilder optional_builder(SemanticTableRef());
  optional_builder.BuildRequiredMatch(match);
  graph_.optional_matches.push_back(optional_builder.Release());
}

void QueryGraphBuilder::BuildRequiredMatch(const ast::Match &match) {
  if (match.optional_match) {
    CHECK(match.pattern != nullptr, common::InvalidArgumentError,
          Missing("OPTIONAL MATCH pattern"));
  } else {
    CHECK(match.pattern != nullptr, common::InvalidArgumentError,
          Missing("MATCH pattern"));
  }
  AddPatternToQueryGraph(&graph_, *match.pattern);
  if (match.where) {
    AddWhere(match.where.get());
  }
}

std::unordered_set<LogicalVariable> QueryGraphBuilder::CurrentNodeSymbols()
    const {
  std::unordered_set<LogicalVariable> symbols = graph_.argument_ids;
  AddSymbols(&symbols, graph_.pattern_nodes);
  for (const auto &mutating_pattern : graph_.mutating_patterns) {
    switch (mutating_pattern.kind) {
      case MutatingPatternKind::kCreate:
        AddCreatePatternNodeSymbols(&symbols, mutating_pattern.create);
        break;
      case MutatingPatternKind::kMerge:
        AddSymbols(&symbols, mutating_pattern.merge.match_graph.pattern_nodes);
        break;
      case MutatingPatternKind::kSet:
      case MutatingPatternKind::kDelete:
      case MutatingPatternKind::kRemove:
        break;
    }
  }
  return symbols;
}

MutatingPattern QueryGraphBuilder::BuildMutatingPattern(
    const ast::UpdatingClause &clause) const {
  MutatingPattern mutating_pattern;
  mutating_pattern.clause = &clause;
  switch (clause.node_type) {
    case ast::ASTNodeType::kCreate: {
      const auto &create = ast::CastAst<ast::Create>(clause);
      CHECK(create.pattern != nullptr, common::InvalidArgumentError,
            Missing("CREATE pattern"));
      mutating_pattern.kind = MutatingPatternKind::kCreate;
      mutating_pattern.create =
          BuildCreatePattern(*create.pattern, CurrentNodeSymbols());
      return mutating_pattern;
    }
    case ast::ASTNodeType::kMerge: {
      const auto &merge = ast::CastAst<ast::Merge>(clause);
      mutating_pattern.kind = MutatingPatternKind::kMerge;
      mutating_pattern.merge =
          BuildMergePattern(merge, graph_.argument_ids, CurrentNodeSymbols());
      return mutating_pattern;
    }
    case ast::ASTNodeType::kSet: {
      const auto &set = ast::CastAst<ast::Set>(clause);
      mutating_pattern.kind = MutatingPatternKind::kSet;
      mutating_pattern.set_patterns = BuildSetMutatingPatterns(set);
      return mutating_pattern;
    }
    case ast::ASTNodeType::kDelete: {
      const auto &del = ast::CastAst<ast::Delete>(clause);
      mutating_pattern.kind = MutatingPatternKind::kDelete;
      mutating_pattern.delete_patterns.reserve(del.expressions.size());
      for (const auto &expression : del.expressions) {
        CHECK(expression != nullptr, common::InvalidArgumentError,
              Missing("DELETE expression"));
        mutating_pattern.delete_patterns.push_back(
            {.expression = expression.get(), .detach = del.detach});
      }
      return mutating_pattern;
    }
    case ast::ASTNodeType::kRemove: {
      const auto &remove = ast::CastAst<ast::Remove>(clause);
      mutating_pattern.kind = MutatingPatternKind::kRemove;
      mutating_pattern.remove_patterns = BuildRemoveMutatingPatterns(remove);
      return mutating_pattern;
    }
    default:
      break;
  }
  THROW(common::InvalidArgumentError, Unsupported("updating clause"));
}

const ast::SemanticTable &QueryGraphBuilder::SemanticTableRef() const {
  CHECK(semantic_table_ != nullptr, common::InternalError,
        "semantic table is null");
  return *semantic_table_;
}

}  // namespace ir
