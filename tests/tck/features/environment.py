"""Behave lifecycle hooks for the vendored openCypher TCK."""

from __future__ import annotations

import os
import re
from pathlib import Path

from behave.model import ScenarioOutline
from neo4j import GraphDatabase


def before_all(context):
    excluded = _exclude_unsupported_scenarios(context._runner.features)
    if excluded:
        print(f"TCK collection excluded {excluded} unsupported scenarios")

    uri = os.environ.get("ROCKSGRAPH_TCK_URI", "bolt://127.0.0.1:7687")
    user = os.environ.get("ROCKSGRAPH_TCK_USER", "neo4j")
    password = os.environ.get("ROCKSGRAPH_TCK_PASSWORD", "password")
    context.tck_root = Path(__file__).resolve().parents[1]
    context.driver = GraphDatabase.driver(uri, auth=(user, password))
    context.driver.verify_connectivity()


def _expects_second_precision_offset(scenario):
    pattern = re.compile(r"[+-]\d{2}:\d{2}:\d{2}(?:\[|\]|')")
    for step in scenario.steps:
        table = getattr(step, "table", None)
        if table is None:
            continue
        cells = list(table.headings)
        for row in table.rows:
            cells.extend(row.cells)
        if any(pattern.search(cell) for cell in cells):
            return True
    return False


def _unsupported_reason(scenario):
    if any(
        step.name.startswith("there exists a procedure ")
        for step in scenario.steps
    ):
        # The TCK assumes that its harness can register arbitrary procedures
        # for a scenario.  RocksGraph intentionally exposes no such server API,
        # so executing these scenarios would only test the missing fixture.
        return "dynamic TCK procedures are not supported"
    if _expects_second_precision_offset(scenario):
        # The Neo4j Python driver hydrates numeric Bolt offsets through a
        # minute-precision tzinfo object.  The server preserves offset seconds,
        # but this adapter cannot observe them for result comparison.
        return "Neo4j Python driver truncates UTC offsets to minutes"
    return None


def _exclude_from_container(container):
    excluded = 0
    retained = []
    for run_item in container.run_items:
        if hasattr(run_item, "run_items"):
            excluded += _exclude_from_container(run_item)
            if run_item.run_items:
                retained.append(run_item)
        elif isinstance(run_item, ScenarioOutline):
            scenarios = run_item.scenarios
            supported = [
                scenario
                for scenario in scenarios
                if _unsupported_reason(scenario) is None
            ]
            excluded += len(scenarios) - len(supported)
            scenarios[:] = supported
            if scenarios:
                retained.append(run_item)
        elif _unsupported_reason(run_item) is None:
            retained.append(run_item)
        else:
            excluded += 1

    container.run_items[:] = retained
    container.scenarios[:] = [
        scenario for scenario in container.scenarios if scenario in retained
    ]
    if hasattr(container, "rules"):
        container.rules[:] = [
            rule for rule in container.rules if rule in retained
        ]
    return excluded


def _exclude_unsupported_scenarios(features):
    excluded = 0
    retained = []
    for feature in features:
        excluded += _exclude_from_container(feature)
        if feature.run_items:
            retained.append(feature)
    features[:] = retained
    return excluded


def before_scenario(context, scenario):
    context.session = context.driver.session(
        database=os.environ.get("ROCKSGRAPH_TCK_DATABASE", "default")
    )
    context.parameters = {}
    context.actual = None
    context.expected_effects = None
    context.before_snapshot = None
    context.last_error = None
    context.last_query = None
    # Each TCK scenario is isolated.  This also makes "any graph" deterministic
    # for a server that does not provide a separate database per test.
    context.session.run("MATCH (n) DETACH DELETE n").consume()


def after_scenario(context, scenario):
    if getattr(context, "session", None) is not None:
        context.session.close()


def after_all(context):
    if getattr(context, "driver", None) is not None:
        context.driver.close()
