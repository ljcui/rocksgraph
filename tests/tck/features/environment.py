"""Behave lifecycle hooks for the vendored openCypher TCK."""

from __future__ import annotations

import os
import re
from pathlib import Path

from neo4j import GraphDatabase


def before_all(context):
    uri = os.environ.get("ROCKSGRAPH_TCK_URI", "bolt://127.0.0.1:7687")
    user = os.environ.get("ROCKSGRAPH_TCK_USER", "neo4j")
    password = os.environ.get("ROCKSGRAPH_TCK_PASSWORD", "password")
    context.tck_root = Path(__file__).resolve().parents[1]
    context.driver = GraphDatabase.driver(uri, auth=(user, password))
    context.driver.verify_connectivity()
    context.manifest = set()
    if os.environ.get("ROCKSGRAPH_TCK_SCOPE", "all") == "manifest":
        context.manifest = _read_manifest(
            context.tck_root / "tck_scenario_manifest.tsv"
        )


def _read_manifest(path):
    selections = set()
    if not path.exists():
        return selections
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        feature, name = line.split("\t", 1)
        selections.add((feature.strip(), name.strip()))
    return selections


def _manifest_key(context, scenario):
    feature = Path(scenario.feature.filename)
    try:
        feature = feature.relative_to("features")
    except ValueError:
        feature = Path(feature.name)
    name = scenario.name.split(" -- @", 1)[0]
    return str(feature), name


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


def before_scenario(context, scenario):
    if os.environ.get("ROCKSGRAPH_TCK_SCOPE", "all") == "manifest":
        if _manifest_key(context, scenario) not in context.manifest:
            scenario.skip("not selected by tck_scenario_manifest.tsv")
            return
    if any(
        step.name.startswith("there exists a procedure ")
        for step in scenario.steps
    ):
        # The TCK assumes that its harness can register arbitrary procedures
        # for a scenario.  RocksGraph intentionally exposes no such server API,
        # so executing these scenarios would only test the missing fixture.
        scenario.skip("dynamic TCK procedures are not supported")
        return
    if _expects_second_precision_offset(scenario):
        # The Neo4j Python driver hydrates numeric Bolt offsets through a
        # minute-precision tzinfo object.  The server preserves offset seconds,
        # but this adapter cannot observe them for result comparison.
        scenario.skip("Neo4j Python driver truncates UTC offsets to minutes")
        return
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
