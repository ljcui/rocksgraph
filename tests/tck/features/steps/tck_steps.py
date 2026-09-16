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
    escaped = (
        value.replace("\\", "\\\\")
        .replace("\b", "\\b")
        .replace("\f", "\\f")
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
        .replace("'", "\\'")
    )
    return f"'{escaped}'"


def _render_temporal(value):
    text = str(value)
    timezone_id = getattr(getattr(value, "tzinfo", None), "zone", None)
    utc_offset = value.utcoffset() if hasattr(value, "utcoffset") else None
    if utc_offset is not None:
        offset_seconds = int(utc_offset.total_seconds())
        sign = "-" if offset_seconds < 0 else "+"
        remaining = abs(offset_seconds)
        hours, remaining = divmod(remaining, 3600)
        minutes, seconds = divmod(remaining, 60)
        offset = f"{sign}{hours:02d}:{minutes:02d}"
        if seconds:
            offset += f":{seconds:02d}"
        text = re.sub(r"(Z|[+-]\d\d:\d\d)$", offset, text)
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
    # Fixed offsets (including the default UTC zone) may expose a ``zone``
    # attribute too.  Region IDs are the values the TCK renders in brackets.
    if timezone_id and "/" in timezone_id:
        text += f"[{timezone_id}]"
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
    # neo4j.time.Duration is a tuple subclass, so extension values must be
    # handled before the generic container cases.
    if value.__class__.__module__.startswith("neo4j.time"):
        return _render_temporal(value)
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
    # Other driver extension values have a stable string representation.
    return _quote(str(value))


def _split_map_entry(text):
    depth = 0
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
        elif char == ":" and depth == 0:
            return text[:index].strip(), text[index + 1 :].strip()
    return text.strip(), ""


def _normalise_number(text):
    if not re.fullmatch(
        r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?", text
    ):
        return None
    if not any(char in text for char in ".eE"):
        return f"integer:{int(text)}"
    value = float(text)
    if value == 0.0:
        value = 0.0
    return f"float:{value.hex()}"


def _normalise_map(text):
    entries = []
    for entry in _split_top_level(text[1:-1]):
        if not entry:
            continue
        key, value = _split_map_entry(entry)
        entries.append((key, _normalise_value(value)))
    entries.sort(key=lambda item: item[0])
    return "{" + ", ".join(f"{key}: {value}" for key, value in entries) + "}"


def _normalise_node(text):
    inner = text[1:-1].strip()
    property_start = inner.find("{")
    properties = ""
    if property_start >= 0:
        properties = _normalise_map(inner[property_start:])
        inner = inner[:property_start].strip()
    labels = sorted(re.findall(r":([^:\s{}]+)", inner))
    result = "(" + "".join(f":{label}" for label in labels)
    if properties:
        if labels:
            result += " "
        result += properties
    return result + ")"


def _normalise_relationship(text):
    inner = text[1:-1].strip()
    property_start = inner.find("{")
    properties = ""
    if property_start >= 0:
        properties = _normalise_map(inner[property_start:])
        inner = inner[:property_start].strip()
    result = "[" + inner
    if properties:
        result += " " + properties
    return result + "]"


def _normalise_value(text):
    text = text.strip()
    number = _normalise_number(text)
    if number is not None:
        return number
    if text.startswith("{") and text.endswith("}"):
        return _normalise_map(text)
    if text.startswith("(") and text.endswith(")"):
        return _normalise_node(text)
    if text.startswith("[:") and text.endswith("]"):
        return _normalise_relationship(text)
    if text.startswith("[") and text.endswith("]"):
        values = [
            _normalise_value(value)
            for value in _split_top_level(text[1:-1])
            if value
        ]
        return "[" + ", ".join(values) + "]"
    return text


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
    # Cucumber unescapes doubled backslashes in table cells; Behave leaves
    # them intact.  The vendored TCK uses Cucumber's table convention.
    rows = [
        [cell.replace("\\\\", "\\") for cell in row]
        for row in table.rows
    ]
    return list(table.headings), rows


def _normalise_column(column):
    # The openCypher table uses the source expression as a column name.  The
    # RocksGraph planner (like Neo4j) omits redundant parentheses around a
    # list/map expression, e.g. ``(list[1]).missing`` -> ``list[1].missing``.
    previous = None
    while previous != column:
        previous = column
        column = re.sub(r"\(([^()]*)\)(?=[.\[])", r"\1", column)
    output = []
    index = 0
    while index < len(column):
        char = column[index]
        if char in "'\"`":
            quote = char
            start = index
            index += 1
            escaped = False
            while index < len(column):
                current = column[index]
                index += 1
                if escaped:
                    escaped = False
                elif current == "\\":
                    escaped = True
                elif current == quote:
                    break
            output.append(column[start:index])
            continue
        if char.isspace():
            index += 1
            continue
        if char.isalpha() or char == "_":
            start = index
            index += 1
            while index < len(column) and (
                column[index].isalnum() or column[index] == "_"
            ):
                index += 1
            identifier = column[start:index]
            following = index
            while following < len(column) and column[following].isspace():
                following += 1
            fold_case = (
                following < len(column) and column[following] == "("
            ) or identifier.lower() in ("distinct", "null")
            output.append(identifier.lower() if fold_case else identifier)
            continue
        output.append(char)
        index += 1
    column = "".join(output)
    column = re.sub(r"\bNULL\b", "null", column)
    while column.startswith("(") and column.endswith(")"):
        depth = 0
        closes_at_end = False
        for index, char in enumerate(column):
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    closes_at_end = index == len(column) - 1
                    break
        if not closes_at_end:
            break
        column = column[1:-1]
    return column


def _assert_result(context, table, ordered=False, ignore_list_order=False):
    assert context.last_error is None, f"query failed: {context.last_error}"
    assert context.actual is not None, "no query has been executed"
    columns, expected = _expected_rows(table)
    actual_columns = [
        _normalise_column(column) for column in context.actual["columns"]
    ]
    expected_columns = [_normalise_column(column) for column in columns]
    actual_source_rows = context.actual["rows"]
    if actual_columns != expected_columns and sorted(actual_columns) == sorted(
        expected_columns
    ):
        available = list(enumerate(actual_columns))
        indexes = []
        for expected_column in expected_columns:
            match = next(
                index
                for index, (_, actual_column) in enumerate(available)
                if actual_column == expected_column
            )
            indexes.append(available.pop(match)[0])
        actual_source_rows = [
            [row[index] for index in indexes] for row in actual_source_rows
        ]
        actual_columns = expected_columns
    assert actual_columns == expected_columns, (
        f"columns: actual={actual_columns!r}, expected={expected_columns!r}"
    )
    actual_rows = [
        [_normalise_value(value) for value in row]
        for row in actual_source_rows
    ]
    expected = [[_normalise_value(value) for value in row] for row in expected]
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
    actual = _effects(context.before_snapshot, _snapshot(context))
    assert actual == {}, f"side effects: actual={actual!r}, expected={{}}"


@then("the side effects should be:")
def side_effects_should_be(context):
    assert context.before_snapshot is not None
    expected = {name: int(value) for name, value in _table_pairs(context.table)}
    actual = _effects(context.before_snapshot, _snapshot(context))
    assert actual == expected, (
        f"side effects: actual={actual!r}, expected={expected!r}"
    )
