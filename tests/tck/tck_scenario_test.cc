#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
#include "common/exception.h"
#include "ir/query_ir.h"
#include "planner/logical_plan_builder.h"
#include "runtime/query_executor.h"
#include "storage/in_memory_graph.h"
#include "value/value.h"

namespace {

struct Table {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;
};

struct Scenario {
  std::string name;
  bool outline = false;
  std::string graph;
  std::vector<std::string> setup_queries;
  Table parameters;
  std::string query;
  bool ordered = false;
  bool empty_result = false;
  Table result;
  std::optional<std::string> error_phase;
  std::map<std::string, std::int64_t> side_effects;
  Table examples;
};

struct GraphSnapshot {
  std::set<std::string> nodes;
  std::set<std::string> relationships;
  std::set<std::string> properties;
  std::set<std::string> labels;
};

std::string Trim(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size() &&
         std::isspace(static_cast<unsigned char>(text[first]))) {
    ++first;
  }
  std::size_t last = text.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(text[last - 1]))) {
    --last;
  }
  return std::string(text.substr(first, last - first));
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.starts_with(prefix);
}

std::vector<std::string> ParseTableRow(std::string_view line) {
  const std::string text = Trim(line);
  if (text.size() < 2 || text.front() != '|' || text.back() != '|') {
    return {};
  }
  std::vector<std::string> cells;
  std::string current;
  bool escaped = false;
  for (std::size_t index = 1; index + 1 < text.size(); ++index) {
    const char character = text[index];
    if (escaped) {
      current.push_back(character == 'n' ? '\n' : character);
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == '|') {
      cells.push_back(Trim(current));
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  cells.push_back(Trim(current));
  return cells;
}

Table ParseTable(const std::vector<std::string> &lines, std::size_t *index) {
  Table table;
  while (*index < lines.size() && Trim(lines[*index]).empty()) {
    ++*index;
  }
  if (*index >= lines.size()) {
    return table;
  }
  table.header = ParseTableRow(lines[*index]);
  if (table.header.empty()) {
    return table;
  }
  ++*index;
  while (*index < lines.size()) {
    std::vector<std::string> row = ParseTableRow(lines[*index]);
    if (row.empty()) {
      break;
    }
    table.rows.push_back(std::move(row));
    ++*index;
  }
  return table;
}

std::string ParseDocString(const std::vector<std::string> &lines,
                           std::size_t *index) {
  while (*index < lines.size() && Trim(lines[*index]) != "\"\"\"") {
    ++*index;
  }
  if (*index == lines.size()) {
    return {};
  }
  ++*index;
  std::vector<std::string> content;
  while (*index < lines.size() && Trim(lines[*index]) != "\"\"\"") {
    content.push_back(lines[*index]);
    ++*index;
  }
  if (*index < lines.size()) {
    ++*index;
  }
  std::size_t indentation = std::string::npos;
  for (const auto &line : content) {
    if (Trim(line).empty()) {
      continue;
    }
    indentation = std::min(indentation, line.find_first_not_of(" \t"));
  }
  std::string result;
  for (std::size_t line_index = 0; line_index < content.size(); ++line_index) {
    if (line_index > 0) {
      result.push_back('\n');
    }
    if (indentation != std::string::npos &&
        content[line_index].size() >= indentation) {
      result += content[line_index].substr(indentation);
    }
  }
  return Trim(result);
}

std::vector<std::string> ReadLines(const std::filesystem::path &path) {
  std::ifstream stream(path);
  EXPECT_TRUE(stream.is_open()) << path;
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::vector<Scenario> ParseScenarios(const std::filesystem::path &path) {
  const std::vector<std::string> lines = ReadLines(path);
  std::vector<Scenario> scenarios;
  std::size_t index = 0;
  while (index < lines.size()) {
    const std::string line = Trim(lines[index]);
    bool outline = false;
    std::string name;
    if (StartsWith(line, "Scenario Outline:")) {
      outline = true;
      name = Trim(line.substr(std::string_view("Scenario Outline:").size()));
    } else if (StartsWith(line, "Scenario:")) {
      name = Trim(line.substr(std::string_view("Scenario:").size()));
    } else {
      ++index;
      continue;
    }

    Scenario scenario{.name = std::move(name), .outline = outline};
    ++index;
    while (index < lines.size()) {
      const std::string step = Trim(lines[index]);
      if (StartsWith(step, "Scenario:") ||
          StartsWith(step, "Scenario Outline:")) {
        break;
      }
      if (StartsWith(step, "Given an empty graph")) {
        scenario.graph = "empty";
        ++index;
      } else if (StartsWith(step, "Given any graph")) {
        scenario.graph = "any";
        ++index;
      } else if (StartsWith(step, "Given the ") && step.ends_with(" graph")) {
        scenario.graph =
            step.substr(std::string_view("Given the ").size(),
                        step.size() - std::string_view("Given the ").size() -
                            std::string_view(" graph").size());
        ++index;
      } else if (step == "And having executed:" ||
                 step == "And after having executed:") {
        ++index;
        scenario.setup_queries.push_back(ParseDocString(lines, &index));
      } else if (step == "When executing query:") {
        ++index;
        scenario.query = ParseDocString(lines, &index);
      } else if (step == "And parameters are:" ||
                 step == "And parameter values are:") {
        ++index;
        scenario.parameters = ParseTable(lines, &index);
      } else if (step == "Then the result should be empty") {
        scenario.empty_result = true;
        ++index;
      } else if (step == "Then the result should be, in order:") {
        scenario.ordered = true;
        ++index;
        scenario.result = ParseTable(lines, &index);
      } else if (step == "Then the result should be, in any order:") {
        ++index;
        scenario.result = ParseTable(lines, &index);
      } else if (StartsWith(step, "Then a ") || StartsWith(step, "Then an ")) {
        if (step.find("at compile time") != std::string::npos) {
          scenario.error_phase = "compile time";
        } else if (step.find("at runtime") != std::string::npos) {
          scenario.error_phase = "runtime";
        }
        ++index;
      } else if (step == "And no side effects") {
        scenario.side_effects.clear();
        ++index;
      } else if (step == "And the side effects should be:") {
        ++index;
        const Table effects = ParseTable(lines, &index);
        if (effects.header.size() == 2) {
          scenario.side_effects[effects.header[0]] =
              std::stoll(effects.header[1]);
        }
        for (const auto &row : effects.rows) {
          if (row.size() == 2) {
            scenario.side_effects[row[0]] = std::stoll(row[1]);
          }
        }
      } else if (StartsWith(step, "Examples:") ||
                 (StartsWith(step, "Examples ") && step.ends_with(':'))) {
        ++index;
        scenario.examples = ParseTable(lines, &index);
      } else {
        ++index;
      }
    }
    scenarios.push_back(std::move(scenario));
  }
  return scenarios;
}

std::string Substitute(std::string text,
                       const std::map<std::string, std::string> &values) {
  for (const auto &[name, value] : values) {
    const std::string placeholder = "<" + name + ">";
    std::size_t position = 0;
    while ((position = text.find(placeholder, position)) != std::string::npos) {
      text.replace(position, placeholder.size(), value);
      position += value.size();
    }
  }
  return text;
}

Table SubstituteTable(Table table,
                      const std::map<std::string, std::string> &values) {
  for (auto &cell : table.header) {
    cell = Substitute(std::move(cell), values);
  }
  for (auto &row : table.rows) {
    for (auto &cell : row) {
      cell = Substitute(std::move(cell), values);
    }
  }
  return table;
}

Scenario SubstituteScenario(Scenario scenario,
                            const std::map<std::string, std::string> &values) {
  scenario.name = Substitute(std::move(scenario.name), values);
  for (auto &query : scenario.setup_queries) {
    query = Substitute(std::move(query), values);
  }
  scenario.query = Substitute(std::move(scenario.query), values);
  scenario.parameters = SubstituteTable(std::move(scenario.parameters), values);
  scenario.result = SubstituteTable(std::move(scenario.result), values);
  return scenario;
}

std::vector<Scenario> ExpandScenario(const Scenario &scenario) {
  if (!scenario.outline) {
    return {scenario};
  }
  std::vector<Scenario> expanded;
  for (const auto &row : scenario.examples.rows) {
    EXPECT_EQ(row.size(), scenario.examples.header.size());
    if (row.size() != scenario.examples.header.size()) {
      continue;
    }
    std::map<std::string, std::string> values;
    for (std::size_t index = 0; index < row.size(); ++index) {
      values[scenario.examples.header[index]] = row[index];
    }
    expanded.push_back(SubstituteScenario(scenario, values));
  }
  return expanded;
}

rg::QueryOptions QueryOptionsFor(const rg::InMemoryGraph &graph,
                                 rg::QueryParameters parameters = {}) {
  return {.planner_statistics = &graph,
          .planner_catalog = &graph,
          .parameters = std::move(parameters)};
}

rg::Value ParseValue(const std::string &text, rg::InMemoryGraph *graph) {
  rg::QueryResult result = rg::ExecuteReadQuery(
      *graph, "RETURN " + text + " AS value", QueryOptionsFor(*graph));
  EXPECT_EQ(result.rows.size(), 1U) << text;
  EXPECT_EQ(result.rows[0].size(), 1U) << text;
  return result.rows[0][0];
}

rg::QueryParameters ParseParameters(const Table &table,
                                    rg::InMemoryGraph *graph) {
  rg::QueryParameters parameters;
  if (table.header.empty()) {
    return parameters;
  }
  if (table.header.size() == 2) {
    parameters[table.header[0]] = ParseValue(table.header[1], graph);
  }
  for (const auto &row : table.rows) {
    EXPECT_EQ(row.size(), 2U);
    if (row.size() != 2) {
      continue;
    }
    parameters[row[0]] = ParseValue(row[1], graph);
  }
  return parameters;
}

std::string QuoteString(std::string_view value) {
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\\' || character == '\'') {
      quoted.push_back('\\');
    }
    quoted.push_back(character);
  }
  quoted.push_back('\'');
  return quoted;
}

std::string FormatValue(const rg::Value &value);

std::string FormatMap(const rg::Value::Map &map) {
  std::string result = "{";
  bool first = true;
  for (const auto &[key, value] : map) {
    if (!first) {
      result += ", ";
    }
    first = false;
    result += key + ": " + FormatValue(value);
  }
  return result + "}";
}

std::string FormatNode(const rg::Node &node) {
  std::string result = "(";
  for (const auto &label : node.labels) {
    result += ":" + label;
  }
  if (!node.properties.empty()) {
    if (!node.labels.empty()) {
      result.push_back(' ');
    }
    result += FormatMap(node.properties);
  }
  return result + ")";
}

std::string FormatRelationship(const rg::Relationship &relationship) {
  std::string result = "[:" + relationship.type;
  if (!relationship.properties.empty()) {
    result += " " + FormatMap(relationship.properties);
  }
  return result + "]";
}

std::string FormatPath(const rg::Path &path) {
  if (path.nodes.empty() ||
      path.nodes.size() != path.relationships.size() + 1 ||
      path.nodes.front() == nullptr) {
    return "<>";
  }
  std::string result = "<" + FormatNode(*path.nodes.front());
  for (std::size_t index = 0; index < path.relationships.size(); ++index) {
    const auto &relationship = path.relationships[index];
    const auto &next_node = path.nodes[index + 1];
    if (relationship == nullptr || next_node == nullptr) {
      return "<>";
    }
    const bool outgoing =
        relationship->start_node_id == path.nodes[index]->id &&
        relationship->end_node_id == next_node->id;
    result += outgoing ? "-" : "<-";
    result += FormatRelationship(*relationship);
    result += outgoing ? "->" : "-";
    result += FormatNode(*next_node);
  }
  return result + ">";
}

std::string FormatValue(const rg::Value &value) {
  if (value.IsNull() || value.IsBool() || value.IsInteger()) {
    return value.ToString();
  }
  if (value.IsDouble()) {
    if (std::isnan(value.AsDouble())) {
      return "NaN";
    }
    if (std::isinf(value.AsDouble())) {
      return value.AsDouble() < 0 ? "-Inf" : "Inf";
    }
    std::ostringstream stream;
    if (std::trunc(value.AsDouble()) == value.AsDouble()) {
      stream << std::fixed << std::setprecision(1) << value.AsDouble();
    } else {
      stream << value.AsDouble();
    }
    return stream.str();
  }
  if (value.IsString()) {
    return QuoteString(value.AsString());
  }
  if (value.IsList()) {
    std::string result = "[";
    for (std::size_t index = 0; index < value.AsList().size(); ++index) {
      if (index > 0) {
        result += ", ";
      }
      result += FormatValue(value.AsList()[index]);
    }
    return result + "]";
  }
  if (value.IsMap()) {
    return FormatMap(value.AsMap());
  }
  if (value.IsNode()) {
    return FormatNode(value.AsNode());
  }
  if (value.IsRelationship()) {
    return FormatRelationship(value.AsRelationship());
  }
  if (value.IsPath()) {
    return FormatPath(value.AsPath());
  }
  if (value.IsDate() || value.IsLocalTime() || value.IsTime() ||
      value.IsLocalDateTime() || value.IsDateTime() || value.IsDuration()) {
    return QuoteString(value.ToString());
  }
  return value.ToString();
}

std::vector<std::vector<std::string>> FormatRows(
    const rg::QueryResult &result) {
  std::vector<std::vector<std::string>> rows;
  for (const auto &row : result.rows) {
    std::vector<std::string> formatted;
    for (const auto &value : row) {
      formatted.push_back(FormatValue(value));
    }
    rows.push_back(std::move(formatted));
  }
  return rows;
}

GraphSnapshot Snapshot(const rg::InMemoryGraph &graph) {
  GraphSnapshot snapshot;
  for (const auto &node : graph.Nodes()) {
    const std::string entity = "node:" + std::to_string(node->id);
    snapshot.nodes.insert(entity);
    for (const auto &label : node->labels) {
      snapshot.labels.insert(label);
    }
    for (const auto &[key, value] : node->properties) {
      snapshot.properties.insert(entity + ":" + key + ":" +
                                 rg::ValueKey(value));
    }
  }
  for (const auto &relationship : graph.Relationships()) {
    const std::string entity =
        "relationship:" + std::to_string(relationship->id);
    snapshot.relationships.insert(entity);
    for (const auto &[key, value] : relationship->properties) {
      snapshot.properties.insert(entity + ":" + key + ":" +
                                 rg::ValueKey(value));
    }
  }
  return snapshot;
}

std::map<std::string, std::int64_t> SideEffects(const GraphSnapshot &before,
                                                const GraphSnapshot &after) {
  const auto add_effect = [](std::map<std::string, std::int64_t> *effects,
                             std::string_view name, const auto &old_values,
                             const auto &new_values) {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::set_difference(new_values.begin(), new_values.end(),
                        old_values.begin(), old_values.end(),
                        std::back_inserter(added));
    std::set_difference(old_values.begin(), old_values.end(),
                        new_values.begin(), new_values.end(),
                        std::back_inserter(removed));
    if (!added.empty()) {
      (*effects)["+" + std::string(name)] =
          static_cast<std::int64_t>(added.size());
    }
    if (!removed.empty()) {
      (*effects)["-" + std::string(name)] =
          static_cast<std::int64_t>(removed.size());
    }
  };
  std::map<std::string, std::int64_t> effects;
  add_effect(&effects, "nodes", before.nodes, after.nodes);
  add_effect(&effects, "relationships", before.relationships,
             after.relationships);
  add_effect(&effects, "properties", before.properties, after.properties);
  add_effect(&effects, "labels", before.labels, after.labels);
  return effects;
}

void RunScenario(const Scenario &scenario) {
  SCOPED_TRACE(scenario.name);
  ASSERT_TRUE(scenario.graph == "empty" || scenario.graph == "any")
      << "named graph loading is not implemented: " << scenario.graph;
  rg::InMemoryGraph graph;
  for (const auto &setup : scenario.setup_queries) {
    (void)rg::ExecuteQuery(graph, setup, QueryOptionsFor(graph));
  }
  const rg::QueryParameters parameters =
      ParseParameters(scenario.parameters, &graph);
  const GraphSnapshot before = Snapshot(graph);

  if (scenario.error_phase.has_value()) {
    std::unique_ptr<ir::LogicalPlan> plan;
    bool compile_failed = false;
    try {
      std::unique_ptr<ast::Statement> statement =
          ast::ParseCypherAndRewrite(scenario.query);
      std::unique_ptr<ir::QueryIR> query_ir = ir::CreateQueryIR(*statement);
      plan = ir::CreateLogicalPlan(
          *query_ir, {.planner_statistics = &graph, .planner_catalog = &graph});
    } catch (const common::Exception &) {
      compile_failed = true;
    }
    if (*scenario.error_phase == "compile time") {
      EXPECT_TRUE(compile_failed);
    } else {
      ASSERT_FALSE(compile_failed);
      ASSERT_NE(plan, nullptr);
      EXPECT_THROW((void)rg::QueryExecutor(graph).Execute(*plan, parameters),
                   common::Exception);
    }
    EXPECT_EQ(Snapshot(graph).nodes, before.nodes);
    EXPECT_EQ(Snapshot(graph).relationships, before.relationships);
    EXPECT_EQ(Snapshot(graph).properties, before.properties);
    EXPECT_EQ(Snapshot(graph).labels, before.labels);
    return;
  }
  rg::QueryResult actual = rg::ExecuteQuery(graph, scenario.query,
                                            QueryOptionsFor(graph, parameters));
  EXPECT_EQ(actual.columns, scenario.result.header);
  std::vector<std::vector<std::string>> actual_rows = FormatRows(actual);
  std::vector<std::vector<std::string>> expected_rows = scenario.result.rows;
  if (!scenario.ordered) {
    std::sort(actual_rows.begin(), actual_rows.end());
    std::sort(expected_rows.begin(), expected_rows.end());
  }
  EXPECT_EQ(actual_rows, expected_rows);
  if (scenario.empty_result) {
    EXPECT_TRUE(actual.rows.empty());
  }
  EXPECT_EQ(SideEffects(before, Snapshot(graph)), scenario.side_effects);
}

struct ScenarioSelection {
  std::string feature;
  std::string name;
};

std::vector<ScenarioSelection> ReadManifest(
    const std::filesystem::path &manifest_path) {
  std::ifstream stream(manifest_path);
  EXPECT_TRUE(stream.is_open()) << manifest_path;
  std::vector<ScenarioSelection> selections;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed.starts_with('#')) {
      continue;
    }
    const std::size_t separator = trimmed.find('\t');
    EXPECT_NE(separator, std::string::npos)
        << "malformed TCK manifest line: " << line;
    if (separator == std::string::npos) {
      continue;
    }
    const std::string feature = Trim(trimmed.substr(0, separator));
    const std::string name = Trim(trimmed.substr(separator + 1));
    EXPECT_FALSE(feature.empty()) << "TCK manifest feature is empty";
    EXPECT_FALSE(name.empty()) << "TCK manifest scenario is empty";
    if (!feature.empty() && !name.empty()) {
      selections.push_back({.feature = feature, .name = name});
    }
  }
  return selections;
}

struct TckInventory {
  std::size_t feature_count = 0;
  std::size_t scenario_count = 0;
  std::size_t expanded_scenario_count = 0;
  std::size_t scenario_outline_count = 0;
};

TckInventory CollectInventory(const std::filesystem::path &root) {
  TckInventory inventory;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".feature") {
      continue;
    }
    ++inventory.feature_count;
    const std::vector<Scenario> scenarios = ParseScenarios(entry.path());
    inventory.scenario_count += scenarios.size();
    for (const auto &scenario : scenarios) {
      if (scenario.outline) {
        ++inventory.scenario_outline_count;
      }
      inventory.expanded_scenario_count += ExpandScenario(scenario).size();
    }
  }
  return inventory;
}

TEST(TckScenarioTest, ExecutesConformanceManifest) {
  const std::filesystem::path root = ROCKSGRAPH_TCK_DIR;
  if (!std::filesystem::exists(root / "features")) {
    GTEST_SKIP() << "openCypher TCK checkout not found at " << root;
  }

  const TckInventory inventory = CollectInventory(root / "features");
  ASSERT_GT(inventory.feature_count, 0U);
  ASSERT_GT(inventory.scenario_count, 0U);

  const std::filesystem::path manifest_path = ROCKSGRAPH_TCK_MANIFEST;
  ASSERT_TRUE(std::filesystem::exists(manifest_path));
  const std::vector<ScenarioSelection> selections = ReadManifest(manifest_path);
  ASSERT_FALSE(selections.empty());
  std::set<std::string> manifest_keys;
  for (const auto &selection : selections) {
    const std::string key =
        std::string(selection.feature) + "\n" + std::string(selection.name);
    ASSERT_TRUE(manifest_keys.insert(key).second)
        << "duplicate manifest entry: " << selection.feature << " / "
        << selection.name;
  }
  ASSERT_LE(selections.size(), inventory.expanded_scenario_count);
  RecordProperty("tck_feature_count", std::to_string(inventory.feature_count));
  RecordProperty("tck_scenario_count",
                 std::to_string(inventory.scenario_count));
  RecordProperty("tck_expanded_scenario_count",
                 std::to_string(inventory.expanded_scenario_count));
  RecordProperty("tck_scenario_outline_count",
                 std::to_string(inventory.scenario_outline_count));
  RecordProperty("tck_manifest_entry_count", std::to_string(selections.size()));

  std::map<std::string, std::vector<Scenario>> cache;
  for (const auto &selection : selections) {
    auto [found, inserted] = cache.try_emplace(selection.feature);
    if (inserted) {
      found->second = ParseScenarios(root / "features" / selection.feature);
    }
    const auto scenario =
        std::find_if(found->second.begin(), found->second.end(),
                     [&selection](const Scenario &candidate) {
                       return candidate.name == selection.name;
                     });
    ASSERT_NE(scenario, found->second.end())
        << selection.feature << ": " << selection.name;
    for (const auto &expanded : ExpandScenario(*scenario)) {
      RunScenario(expanded);
    }
  }
}

}  // namespace
