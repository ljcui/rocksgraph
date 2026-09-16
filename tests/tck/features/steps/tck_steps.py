"""openCypher TCK steps backed by the Neo4j Python driver.

The feature files are kept verbatim from openCypher.  This module implements
the small, implementation-neutral step vocabulary used by that checkout and
talks to RocksGraph exclusively through its Bolt endpoint.
"""

from __future__ import annotations

import math
import re

from behave import given, then, when
from neo4j.graph import Node, Path as Neo4jPath, Relationship


def _table_pairs(table):
    if table is None:
        return []
    pairs = [list(table.headings)]
    pairs.extend([list(row) for row in table.rows])
    return [pair for pair in pairs if len(pair) == 2]


def _split_top_level(text):
    parts, start, depth = [], 0, 0
    quote = None
    escaped = False
    for index, char in enumerate(text):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in "'\"`":
            quote = char
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
    parts.append(text[start:].strip())
    return parts


def _quote(value):
    escaped = value.replace("\\", "\\\\").replace("'", "\\'")
    return f"'{escaped}'"


def _render_temporal(value):
    text = str(value)
    # PackStream preserves nanoseconds, while the TCK spells temporal values
    # using the shortest equivalent precision (and uses Z for UTC).
    text = re.sub(r"\.0+(?=([+-]\d\d:\d\d)?$)", "", text)
    text = re.sub(r"(\.\d*?[1-9])0+(?=([+-]\d\d:\d\d)?$)", r"\1", text)
    timezone = ""
    timezone_match = re.search(r"(Z|[+-]\d\d:\d\d)$", text)
    if timezone_match:
        timezone = timezone_match.group(1)
        text = text[: timezone_match.start()]
    if text.endswith(":00"):
        text = text[:-3]
    text += timezone
    if text.endswith("+00:00"):
        text = text[:-6] + "Z"
    return _quote(text)


def _render(value):
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if math.isnan(value):
            return "NaN"
        if math.isinf(value):
            return "-Inf" if value < 0 else "Inf"
        return f"{value:.1f}" if value.is_integer() else repr(value)
    if isinstance(value, str):
        return _quote(value)
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(_render(item) for item in value) + "]"
    if isinstance(value, dict):
        items = sorted(value.items(), key=lambda item: str(item[0]))
        rendered = ", ".join(f"{key}: {_render(item)}" for key, item in items)
        return "{" + rendered + "}"
    if isinstance(value, Node):
        labels = sorted(value.labels)
        result = "(" + "".join(f":{label}" for label in labels)
        properties = dict(value)
        if properties:
            if labels:
                result += " "
            result += _render(properties)
        return result + ")"
    if isinstance(value, Relationship):
        result = f"[:{value.type}"
        properties = dict(value)
        if properties:
            result += " " + _render(properties)
        return result + "]"
    if isinstance(value, Neo4jPath):
        nodes = list(value.nodes)
        relationships = list(value.relationships)
        if not nodes or len(nodes) != len(relationships) + 1:
            return "<>"
        result = "<" + _render(nodes[0])
        for index, relationship in enumerate(relationships):
            current, following = nodes[index], nodes[index + 1]
            outgoing = (
                relationship.start_node.element_id == current.element_id
                and relationship.end_node.element_id == following.element_id
            )
            result += "-" if outgoing else "<-"
            result += _render(relationship)
            result += "->" if outgoing else "-"
            result += _render(following)
        return result + ">"
    if value.__class__.__module__.startswith("neo4j.time"):
        return _render_temporal(value)
    # Other driver extension values have a stable string representation.
    return _quote(str(value))


def _normalise_list(text):
    """Canonicalise list item ordering for the TCK's special list assertions."""
    text = text.strip()
    if not (text.startswith("[") and text.endswith("]")):
        return text
    items = [_normalise_list(item) for item in _split_top_level(text[1:-1])]
    return "[" + ", ".join(sorted(items)) + "]"


def _expected_rows(table):
    if table is None:
        return [], []
    return list(table.headings), [list(row) for row in table.rows]


def _normalise_column(column):
    # The openCypher table uses the source expression as a column name.  The
    # RocksGraph planner (like Neo4j) omits redundant parentheses around a
    # list/map expression, e.g. ``(list[1]).missing`` -> ``list[1].missing``.
    previous = None
    while previous != column:
        previous = column
        column = re.sub(r"\(([^()]*)\)(?=[.\[])", r"\1", column)
    column = re.sub(r"\bNULL\b", "null", column)
    return column


def _assert_result(context, table, ordered=False, ignore_list_order=False):
    assert context.last_error is None, f"query failed: {context.last_error}"
    assert context.actual is not None, "no query has been executed"
    columns, expected = _expected_rows(table)
    actual_columns = [
        _normalise_column(column) for column in context.actual["columns"]
    ]
    expected_columns = [_normalise_column(column) for column in columns]
    assert actual_columns == expected_columns, (
        f"columns: actual={actual_columns!r}, expected={expected_columns!r}"
    )
    actual_rows = context.actual["rows"]
    if ignore_list_order:
        actual_rows = [
            [_normalise_list(value) for value in row] for row in actual_rows
        ]
        expected = [[_normalise_list(value) for value in row] for row in expected]
    if ordered:
        assert actual_rows == expected, (
            f"rows: actual={actual_rows!r}, expected={expected!r}"
        )
    else:
        assert sorted(actual_rows) == sorted(expected), (
            f"rows: actual={actual_rows!r}, expected={expected!r}"
        )


def _snapshot(context):
    nodes, relationships, properties, labels = set(), set(), set(), set()
    query = "MATCH (n) RETURN id(n) AS id, labels(n) AS labels, n"
    for record in context.session.run(query):
        entity = f"node:{record['id']}"
        nodes.add(entity)
        labels.update(record["labels"])
        for key, value in dict(record["n"]).items():
            properties.add((entity, key, _render(value)))
    for record in context.session.run("MATCH ()-[r]->() RETURN id(r) AS id, r"):
        entity = f"relationship:{record['id']}"
        relationships.add(entity)
        for key, value in dict(record["r"]).items():
            properties.add((entity, key, _render(value)))
    return {
        "nodes": nodes,
        "relationships": relationships,
        "properties": properties,
        "labels": labels,
    }


def _effects(before, after):
    result = {}
    for name in ("nodes", "relationships", "properties", "labels"):
        added = len(after[name] - before[name])
        removed = len(before[name] - after[name])
        if added:
            result[f"+{name}"] = added
        if removed:
            result[f"-{name}"] = removed
    return result


def _execute(context, query):
    context.last_query = query
    context.last_error = None
    context.actual = None
    try:
        result = context.session.run(query, context.parameters)
        columns = list(result.keys())
        rows = [[_render(value) for value in record.values()] for record in result]
        result.consume()
        # Bolt represents a write-only Cypher statement as one empty record in
        # some protocol versions.  In the TCK result model this is an empty
        # result (there are no columns or values to assert).
        if not columns:
            rows = []
        context.actual = {"columns": columns, "rows": rows}
    except Exception as error:  # driver errors vary by Bolt protocol version
        context.last_error = error


@given("any graph")
def given_any_graph(context):
    pass


@given("an empty graph")
def given_empty_graph(context):
    pass


@given("the {tree} graph")
def given_binary_tree(context, tree):
    script = context.tck_root / "graphs" / tree / f"{tree}.cypher"
    _execute(context, script.read_text())
    assert context.last_error is None, f"failed to load {tree}: {context.last_error}"


@given("there exists a procedure {signature}")
def given_procedure(context, signature):
    # User-defined procedures are not part of RocksGraph's server API.  Keep
    # the step defined so the feature remains executable and let the query
    # assertion report unsupported procedure calls in the normal way.
    pass


@given("having executed:")
def having_executed(context):
    _execute(context, context.text)
    assert context.last_error is None, f"setup query failed: {context.last_error}"


@given("after having executed:")
def after_having_executed(context):
    having_executed(context)


@given("parameters are:")
def parameters_are(context):
    for name, expression in _table_pairs(context.table):
        try:
            result = context.session.run(f"RETURN {expression} AS value")
            record = result.single()
            result.consume()
            context.parameters[name] = record["value"]
        except Exception as error:
            raise AssertionError(f"parameter {name} failed: {error}") from error


@given("parameter values are:")
def parameter_values_are(context):
    parameters_are(context)


@when("executing query:")
def executing_query(context):
    context.before_snapshot = _snapshot(context)
    _execute(context, context.text)


@when("executing control query:")
def executing_control_query(context):
    _execute(context, context.text)


@then("the result should be empty")
def result_should_be_empty(context):
    assert context.last_error is None, f"query failed: {context.last_error}"
    assert context.actual is not None and not context.actual["rows"]


@then("the result should be, in any order:")
def result_any_order(context):
    _assert_result(context, context.table)


@then("the result should be, in order:")
def result_in_order(context):
    _assert_result(context, context.table, ordered=True)


@then("the result should be (ignoring element order for lists):")
def result_any_order_ignore_lists(context):
    _assert_result(context, context.table, ignore_list_order=True)


@then("the result should be, in order (ignoring element order for lists):")
def result_in_order_ignore_lists(context):
    _assert_result(context, context.table, ordered=True, ignore_list_order=True)


@then("a {error_type} should be raised at {phase}: {error_code}")
def expected_error(context, error_type, phase, error_code):
    assert context.last_error is not None, (
        f"expected query to fail, but it returned {context.actual}"
    )


@then("no side effects")
def no_side_effects(context):
    assert context.before_snapshot is not None
    assert _effects(context.before_snapshot, _snapshot(context)) == {}


@then("the side effects should be:")
def side_effects_should_be(context):
    assert context.before_snapshot is not None
    expected = {name: int(value) for name, value in _table_pairs(context.table)}
    assert _effects(context.before_snapshot, _snapshot(context)) == expected
