"""Behave lifecycle hooks for the vendored openCypher TCK."""

from __future__ import annotations

import os
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


def before_scenario(context, scenario):
    if os.environ.get("ROCKSGRAPH_TCK_SCOPE", "all") == "manifest":
        if _manifest_key(context, scenario) not in context.manifest:
            scenario.skip("not selected by tck_scenario_manifest.tsv")
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
