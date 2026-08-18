"""Tests for the graph agent harness (python3 -m unittest discover -s tests).

The end-to-end cases drive a real Harness over a real git repository with a
fake backend standing in for the coding agent, so the scope fence, the revert
path, the retry and the graph update are all exercised for real.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from agent_harness.backends import AgentSpec, Backend, RunRequest, RunResult
from agent_harness.context import Budget, RepoContext
from agent_harness.graphmodel import Graph, LEAF_FIRST, ROOT_FIRST
from agent_harness.patchformat import FileBlockApplier, parse_blocks
from agent_harness.prompts import PromptBuilder, parse_review
from agent_harness.review import Reviewer
from agent_harness.runner import Harness, HarnessOptions, ON_VIOLATION_RETRY
from agent_harness.scope import ScopeAuditor, ScopeConfig, ScopeResolver
from agent_harness.state import DONE, FAILED, RunState
from agent_harness.stubs import StubIndex, marker_for
from agent_harness.verify import Verifier
from agent_harness.workspace import GitWorkspace

REPO_ROOT = Path(__file__).resolve().parents[1]

TOY_GRAPH = {
    "nodes": [
        {"kind": "Module", "name": "core", "comment": "The foundation module.", "implemented": False},
        {"kind": "Class", "name": "Widget", "comment": "A widget that uses Helper.", "implemented": False},
        {"kind": "Class", "name": "Helper", "comment": "A helper Widget calls.", "implemented": False},
        {"kind": "Class", "name": "Done", "comment": "Already built.", "implemented": True},
    ],
    "edges": [
        {"origin": 0, "destination": 1, "comment": "contains"},
        {"origin": 0, "destination": 2, "comment": "contains"},
        {"origin": 1, "destination": 2, "comment": "uses"},
        {"origin": 1, "destination": 3, "comment": "uses"},
    ],
}


def git(root: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=str(root), check=True, capture_output=True)


class ScriptedBackend(Backend):
    """Stands in for a coding agent: each call writes a scripted set of files."""

    agentic = True

    def __init__(self, root: Path, script):
        super().__init__(AgentSpec(backend="scripted"))
        self.root = root
        self.script = list(script)
        self.prompts: list[str] = []

    def run(self, request: RunRequest) -> RunResult:
        self.prompts.append(request.prompt)
        if not self.script:
            return RunResult(True, text="nothing left to do")
        writes = self.script.pop(0)
        for rel, body in writes.items():
            target = self.root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(body, encoding="utf-8")
        return RunResult(True, text=f"wrote {', '.join(writes)}")


class GraphOrderTests(unittest.TestCase):
    def setUp(self):
        self.graph = Graph.load(REPO_ROOT / "graphs" / "graph_draw.planned.json")

    def test_root_first_places_callers_before_their_dependencies(self):
        order, cyclic = self.graph.order(ROOT_FIRST)
        self.assertEqual(cyclic, [])
        position = {i: n for n, i in enumerate(order)}
        for edge in self.graph.edges:
            self.assertLess(position[edge.origin], position[edge.destination],
                            f"{self.graph.node(edge.origin).name} must precede "
                            f"{self.graph.node(edge.destination).name}")

    def test_leaf_first_is_the_exact_reverse_relation(self):
        order, _ = self.graph.order(LEAF_FIRST)
        position = {i: n for n, i in enumerate(order)}
        for edge in self.graph.edges:
            self.assertLess(position[edge.destination], position[edge.origin])

    def test_owner_module_walks_contains_and_provides_edges(self):
        self.assertEqual(self.graph.owner_module(self.graph.find("TomlDocument::parseText").index).name,
                         "graph_lang_rust")
        self.assertEqual(self.graph.owner_module(self.graph.find("ir::Repo").index).name, "graph_lang")

    def test_pending_skips_implemented_nodes(self):
        order, _ = self.graph.order(ROOT_FIRST)
        pending = self.graph.pending(order)
        self.assertEqual(len(pending), 43)
        self.assertTrue(all(not self.graph.node(i).implemented for i in pending))

    def test_cycles_are_reported_not_dropped(self):
        graph = Graph([], [])
        data = json.loads(json.dumps(TOY_GRAPH))
        data["edges"].append({"origin": 2, "destination": 1, "comment": "uses"})
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "g.json"
            path.write_text(json.dumps(data))
            graph = Graph.load(path)
        order, cyclic = graph.order(ROOT_FIRST)
        self.assertEqual(len(order), len(graph.nodes))
        self.assertTrue(cyclic)


class ScopeTests(unittest.TestCase):
    def setUp(self):
        self.graph = Graph.load(REPO_ROOT / "graphs" / "graph_draw.planned.json")
        self.resolver = ScopeResolver(self.graph, ScopeConfig.load(REPO_ROOT / "harness.scope.json"))

    def test_function_inherits_its_module_directory(self):
        scope = self.resolver.resolve(self.graph.find("TomlDocument::parseText"))
        self.assertIn("graph_lang_rust/**", scope.allow)
        self.assertEqual(scope.module, "graph_lang_rust")

    def test_regression_contract_is_protected_even_inside_an_allowed_tree(self):
        scope = self.resolver.resolve(self.graph.find("graph_draw_tests"))
        self.assertTrue(scope.permits("tests/test_graphbuilder.cpp"))
        self.assertFalse(scope.permits("tests/test_repoanalyzer.cpp"))

    def test_audit_flags_and_returns_offenders(self):
        scope = self.resolver.resolve(self.graph.find("graph_lang"))
        violations, offenders = ScopeAuditor(scope).audit(
            {"graph_lang/sourcetext.cpp": "added",
             "graph_core/graphnode.cpp": "modified",
             "graphs/graph_draw.planned.json": "modified"}, 10)
        self.assertEqual(sorted(offenders),
                         ["graph_core/graphnode.cpp", "graphs/graph_draw.planned.json"])
        self.assertEqual({v.kind for v in violations}, {"out-of-scope", "forbidden"})

    def test_budgets_are_enforced(self):
        scope = self.resolver.resolve(self.graph.find("graph_lang"))
        violations, _ = ScopeAuditor(scope).audit({"graph_lang/a.cpp": "added"}, 99_999)
        self.assertEqual([v.kind for v in violations], ["budget-lines"])


class StubProtocolTests(unittest.TestCase):
    def test_sites_are_found_with_surrounding_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            git(root, "init", "-q")
            (root / "a.cpp").write_text(
                "int caller() {\n"
                f"    // {marker_for('Helper::run')}\n"
                "    return Helper::run();\n"
                "}\n")
            index = StubIndex(root)
            sites = index.sites_for("Helper::run")
            self.assertEqual(len(sites), 1)
            self.assertEqual(sites[0].path, "a.cpp")
            self.assertIn("Helper::run()", sites[0].excerpt)
            self.assertIn("Helper::run", index.outstanding())


class PatchFormatTests(unittest.TestCase):
    def test_blocks_round_trip(self):
        text = "prose\n<<<FILE src/a.cpp>>>\nint a;\n<<<END>>>\n<<<DELETE old.h>>>\n"
        blocks = parse_blocks(text)
        self.assertEqual([b.path for b in blocks], ["src/a.cpp", "old.h"])
        with tempfile.TemporaryDirectory() as tmp:
            written, errors = FileBlockApplier(tmp).apply(blocks)
            self.assertEqual(errors, [])
            self.assertEqual((Path(tmp) / "src/a.cpp").read_text(), "int a;\n")

    def test_escaping_paths_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, errors = FileBlockApplier(tmp).apply(parse_blocks(
                "<<<FILE ../escape.cpp>>>\nx\n<<<END>>>\n"))
            self.assertEqual(len(errors), 1)


class ReviewParsingTests(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(parse_review("looks fine\nVERDICT: PASS\n"), (True, []))

    def test_revise_keeps_short_findings(self):
        passed, findings = parse_review("VERDICT: REVISE\nFINDINGS:\n- duplicate of blankC\n- y\n")
        self.assertFalse(passed)
        self.assertEqual(findings, ["duplicate of blankC", "y"])

    def test_unparseable_reply_does_not_block(self):
        self.assertEqual(parse_review("I could not read the diff"), (True, []))


class HarnessEndToEndTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        git(self.root, "init", "-q")
        git(self.root, "config", "user.email", "harness@test")
        git(self.root, "config", "user.name", "harness")
        (self.root / "core").mkdir()
        (self.root / "core" / "existing.h").write_text("#pragma once\n")
        (self.root / "keepout").mkdir()
        (self.root / "keepout" / "other.cpp").write_text("original\n")
        self.graph_path = self.root / "graph.json"
        self.graph_path.write_text(json.dumps(TOY_GRAPH, indent=2))
        git(self.root, "add", "-A")
        git(self.root, "commit", "-qm", "baseline")
        self.addCleanup(self.tmp.cleanup)

    def _harness(self, script, **option_overrides):
        graph = Graph.load(self.graph_path)
        config = ScopeConfig(shared_allow=(), modules={}, nodes={})
        backend = ScriptedBackend(self.root, script)
        state_dir = self.root / ".harness"
        options = HarnessOptions(order=ROOT_FIRST, attempts=2,
                                 on_violation=ON_VIOLATION_RETRY, **option_overrides)
        prompts = PromptBuilder(graph)
        harness = Harness(
            graph=graph, root=self.root, backend=backend,
            scopes=ScopeResolver(graph, config),
            context=RepoContext(self.root, ignore=config.ignore, budget=Budget()),
            prompts=prompts, workspace=GitWorkspace(self.root, ignore=config.ignore),
            verifier=Verifier([], self.root), reviewer=Reviewer(None, prompts, self.root),
            state=RunState(state_dir / "state.json"), state_dir=state_dir, options=options,
            log=lambda _msg: None,
        )
        return harness, backend, graph

    def test_in_scope_change_marks_the_node_implemented(self):
        harness, _, graph = self._harness([{"core/widget.cpp": "// widget\n"}])
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertTrue(graph.find("Widget").implemented)
        self.assertTrue(json.loads(self.graph_path.read_text())["nodes"][1]["implemented"])

    def test_out_of_scope_edit_is_reverted_and_the_node_retried(self):
        harness, backend, graph = self._harness([
            {"core/widget.cpp": "// first try\n", "keepout/other.cpp": "vandalised\n"},
            {"core/widget.cpp": "// second try\n"},
        ])
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(outcome.attempts, 2)
        self.assertEqual((self.root / "keepout" / "other.cpp").read_text(), "original\n")
        self.assertEqual((self.root / "core" / "widget.cpp").read_text(), "// second try\n")
        self.assertIn("scope fence", backend.prompts[1])
        self.assertIn("RETRY", backend.prompts[1])

    def test_new_out_of_scope_file_is_deleted(self):
        harness, _, graph = self._harness([
            {"core/widget.cpp": "// ok\n", "elsewhere/new.cpp": "sneaky\n"},
            {"core/widget.cpp": "// ok\n"},
        ])
        harness.run_node(graph.find("Widget"))
        self.assertFalse((self.root / "elsewhere" / "new.cpp").exists())

    def test_a_node_that_changes_nothing_fails(self):
        harness, _, graph = self._harness([{}, {}])
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, FAILED)
        self.assertFalse(graph.find("Widget").implemented)

    def test_failing_verification_retries_with_the_output_as_feedback(self):
        harness, backend, graph = self._harness([{"core/widget.cpp": "a\n"},
                                                 {"core/widget.cpp": "b\n"}])
        harness.verifier = Verifier(["test -f core/never_created.cpp"], self.root)
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, FAILED)
        self.assertIn("verification command failed", backend.prompts[1])

    def test_prompt_carries_the_unfilled_call_contract_and_the_call_sites(self):
        harness, backend, graph = self._harness([
            {"core/widget.cpp": f"// {marker_for('Helper')}\nint x;\n"},
            {"core/helper.cpp": "// helper\n"},
        ])
        harness.run_node(graph.find("Widget"))
        widget_prompt = backend.prompts[0]
        self.assertIn(marker_for("Helper"), widget_prompt)
        self.assertIn("Done (Class)", widget_prompt)          # implemented dep, listed as callable
        harness.run_node(graph.find("Helper"))
        helper_prompt = backend.prompts[1]
        self.assertIn("Unfilled calls waiting on YOU", helper_prompt)
        self.assertIn("core/widget.cpp:1", helper_prompt)

    def test_state_survives_a_restart(self):
        harness, _, graph = self._harness([{"core/widget.cpp": "x\n"}])
        harness.run(  [graph.find("Widget").index])
        reloaded = RunState.load(self.root / ".harness" / "state.json")
        self.assertEqual(reloaded.status_of("Widget"), DONE)

    def test_commit_per_node_when_asked(self):
        harness, _, graph = self._harness([{"core/widget.cpp": "x\n"}], commit=True)
        harness.run_node(graph.find("Widget"))
        log = subprocess.run(["git", "log", "--oneline", "-1"], cwd=str(self.root),
                             capture_output=True, text=True).stdout
        self.assertIn("implement Class Widget", log)


if __name__ == "__main__":
    unittest.main(verbosity=2)
