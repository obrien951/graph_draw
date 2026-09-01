"""Tests for the graph agent harness (python3 -m unittest discover -s tests).

The end-to-end cases drive a real Harness over a real git repository with a
fake backend standing in for the coding agent, so the scope fence, the revert
path, the retry and the graph update are all exercised for real.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from unittest.mock import patch

from agent_harness import indexer
from agent_harness.backends import AgentSpec, Backend, RunRequest, RunResult
from agent_harness.context import Budget, RepoContext
from agent_harness.graphmodel import Graph, LEAF_FIRST, ROOT_FIRST
from agent_harness.patchformat import FileBlockApplier, parse_blocks
from agent_harness.prompts import PromptBuilder, parse_review
from agent_harness.review import Reviewer
from agent_harness.runner import Harness, HarnessOptions, ON_VIOLATION_RETRY, SafetyNetLost
from agent_harness.scope import ScopeAuditor, ScopeConfig, ScopeResolver
from agent_harness.state import DONE, FAILED, NOOP, PARTIAL, RunState
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

    def __init__(self, root: Path, script, supports_continue: bool = False):
        super().__init__(AgentSpec(backend="scripted"))
        self.root = root
        self.script = list(script)
        self.prompts: list[str] = []
        self.continued: list[bool] = []
        self.supports_continue = supports_continue

    def run(self, request: RunRequest) -> RunResult:
        self.prompts.append(request.prompt)
        self.continued.append(request.continue_session)
        if not self.script:
            return RunResult(True, text="nothing left to do")
        writes = self.script.pop(0)
        if isinstance(writes, str):
            return RunResult(True, text=writes)  # text-only reply, nothing written
        for rel, body in writes.items():
            target = self.root / rel
            if body is None:  # a fix round deleting what an earlier round wrote
                target.unlink(missing_ok=True)
                continue
            if body == "__RMDIR__":  # simulates `rm -rf .harness/` mid fix-round
                shutil.rmtree(target, ignore_errors=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(body, encoding="utf-8")
        return RunResult(True, text=f"wrote {', '.join(writes)}")


class ReviewBackend(Backend):
    """Stands in for a review model: replies from a script, writes nothing."""

    agentic = True

    def __init__(self, replies, ok=True):
        super().__init__(AgentSpec(backend="review-double"))
        self.replies = list(replies)
        self.ok = ok
        self.calls = 0

    def run(self, request: RunRequest) -> RunResult:
        self.calls += 1
        if not self.ok:
            return RunResult(False, error="reviewer unreachable")
        reply = self.replies.pop(0) if self.replies else "VERDICT: PASS"
        return RunResult(True, text=reply)


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


def _fake_ctags(tmp: Path, *, version_output: str, version_returncode: int = 0,
                 run_stdout: str = "", run_returncode: int = 0) -> Path:
    """An executable standing in for a ctags binary on PATH: prints
    `version_output` for --version, `run_stdout` for any other invocation."""
    script = tmp / "fake-ctags"
    script.write_text(
        "#!/usr/bin/env python3\n"
        "import sys\n"
        f"if '--version' in sys.argv:\n"
        f"    sys.stdout.write({version_output!r})\n"
        f"    sys.exit({version_returncode})\n"
        f"sys.stdout.write({run_stdout!r})\n"
        f"sys.exit({run_returncode})\n"
    )
    script.chmod(0o755)
    return script


class FindCtagsTests(unittest.TestCase):
    def test_recognizes_universal_ctags_by_version_string(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = _fake_ctags(Path(tmp), version_output="Universal Ctags 6.1.0\n")
            self.assertTrue(indexer._is_universal_ctags(str(binary)))

    def test_rejects_a_non_universal_ctags_by_version_string(self):
        with tempfile.TemporaryDirectory() as tmp:
            # this is exactly the failure mode on macOS: /usr/bin/ctags is the
            # Xcode-bundled BSD ctags, which rejects --version-style long
            # options outright rather than printing anything recognizable.
            binary = _fake_ctags(Path(tmp), version_output="", version_returncode=1)
            self.assertFalse(indexer._is_universal_ctags(str(binary)))

    def test_missing_binary_is_not_universal_ctags(self):
        self.assertFalse(indexer._is_universal_ctags("/no/such/ctags-binary"))


class BuildIndexTests(unittest.TestCase):
    def test_returns_none_when_ctags_unavailable(self):
        with patch.object(indexer, "find_ctags", return_value=None):
            self.assertIsNone(indexer.build_index(Path("."), ["a.rs"]))

    def test_returns_none_when_no_files_match_a_supported_language(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = _fake_ctags(Path(tmp), version_output="unused")
            result = indexer.build_index(Path(tmp), ["README.md"], binary=str(binary))
        self.assertIsNone(result)

    def test_parses_ctags_json_output_into_a_line_numbered_listing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout = "\n".join([
                json.dumps({"_type": "tag", "name": "embed", "path": "src/tsne/mod.rs",
                            "kind": "function", "line": 40}),
                json.dumps({"_type": "tag", "name": "pairwise_affinities", "path": "src/tsne/mod.rs",
                            "kind": "function", "line": 12}),
                json.dumps({"_type": "tag", "name": "new", "path": "src/trigram/mod.rs",
                            "kind": "method", "line": 8, "scope": "TrigramProfile"}),
            ]) + "\n"
            binary = _fake_ctags(root, version_output="Universal Ctags 6.1.0\n", run_stdout=stdout)
            result = indexer.build_index(
                root, ["src/tsne/mod.rs", "src/trigram/mod.rs"], binary=str(binary)
            )
        self.assertIsNotNone(result)
        self.assertIn("TrigramProfile::new", result)
        # sorted by file, then by line number within a file
        tsne_header = result.index("src/tsne/mod.rs:")
        first_line = result.index("12\tfunction\tpairwise_affinities")
        second_line = result.index("40\tfunction\tembed")
        self.assertLess(tsne_header, first_line)
        self.assertLess(first_line, second_line)

    def test_nonzero_exit_returns_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = _fake_ctags(root, version_output="Universal Ctags 6.1.0\n", run_returncode=1)
            result = indexer.build_index(root, ["a.rs"], binary=str(binary))
        self.assertIsNone(result)


class SymbolSectionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        git(self.root, "init", "-q")
        git(self.root, "config", "user.email", "harness@test")
        git(self.root, "config", "user.name", "harness")
        (self.root / "src").mkdir()
        (self.root / "src" / "lib.rs").write_text("pub fn embed() {}\n")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-qm", "baseline")
        self.addCleanup(self.tmp.cleanup)

    def test_falls_back_to_regex_scan_when_ctags_unavailable(self):
        with patch.object(indexer, "find_ctags", return_value=None):
            section = RepoContext(self.root, budget=Budget()).symbol_section()
        self.assertNotIn("via ctags", section.title)
        self.assertIn("embed", section.body)

    def test_uses_ctags_output_when_available(self):
        with patch(
            "agent_harness.context.indexer.build_index",
            return_value="src/lib.rs:\n  1\tfunction\tembed",
        ):
            section = RepoContext(self.root, budget=Budget()).symbol_section()
        self.assertIn("via ctags", section.title)
        self.assertIn("1\tfunction\tembed", section.body)


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

    def _harness(self, script, supports_continue: bool = False, **option_overrides):
        graph = Graph.load(self.graph_path)
        config = ScopeConfig(shared_allow=(), modules={}, nodes={})
        backend = ScriptedBackend(self.root, script, supports_continue=supports_continue)
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

    def test_a_failed_node_leaves_nothing_behind(self):
        """A node that never passes must not leave half-written files on disk.

        The 2026-08-19 run left the output of four failed nodes in the tree,
        which is where the duplicate graph_lang_rust/src/ tree came from.
        """
        harness, _, graph = self._harness([{"core/widget.cpp": "a\n"},
                                           {"core/widget.cpp": "b\n"}])
        harness.verifier = Verifier(["test -f core/never_created.cpp"], self.root)
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, FAILED)
        self.assertFalse((self.root / "core" / "widget.cpp").exists())
        self.assertFalse(graph.find("Widget").implemented)

    def test_failed_node_files_are_kept_when_revert_is_off(self):
        harness, _, graph = self._harness([{"core/widget.cpp": "a\n"},
                                           {"core/widget.cpp": "b\n"}],
                                          revert_on_failure=False)
        harness.verifier = Verifier(["test -f core/never_created.cpp"], self.root)
        self.assertEqual(harness.run_node(graph.find("Widget")).status, FAILED)
        self.assertEqual((self.root / "core" / "widget.cpp").read_text(), "b\n")

    def test_a_pre_existing_file_survives_a_failed_node(self):
        """Rewinding restores prior content; it does not blank the file."""
        harness, _, graph = self._harness([{"core/existing.h": "#pragma once\n// vandalised\n"}])
        harness.verifier = Verifier(["false"], self.root)
        self.assertEqual(harness.run_node(graph.find("Widget")).status, FAILED)
        self.assertEqual((self.root / "core" / "existing.h").read_text(), "#pragma once\n")

    def test_partial_marker_is_accepted_when_allowed(self):
        harness, _, graph = self._harness(
            [{"core/widget.cpp": "// HARNESS-PARTIAL(Widget): needs Helper, unbuilt\n"
                                 "void widget() {}\n"}],
            allow_partial=True,
        )
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, PARTIAL)
        self.assertIn("needs Helper", outcome.note)
        self.assertFalse(graph.find("Widget").implemented)
        self.assertEqual((self.root / "core" / "widget.cpp").read_text(),
                         "// HARNESS-PARTIAL(Widget): needs Helper, unbuilt\nvoid widget() {}\n")

    def test_partial_marker_is_rejected_and_retried_when_not_allowed(self):
        harness, backend, graph = self._harness([
            {"core/widget.cpp": "// HARNESS-PARTIAL(Widget): needs Helper\nvoid widget() {}\n"},
            {"core/widget.cpp": "// fully done\nvoid widget() {}\n"},
        ])  # allow_partial defaults False
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(outcome.attempts, 2)
        self.assertIn("HARNESS-PARTIAL", backend.prompts[1])
        self.assertTrue(graph.find("Widget").implemented)

    def test_a_resumed_partial_node_is_told_about_its_own_prior_work(self):
        harness, _, graph = self._harness(
            [{"core/widget.cpp": "// HARNESS-PARTIAL(Widget): needs Helper, unbuilt\n"}],
            allow_partial=True,
        )
        first = harness.run_node(graph.find("Widget"))
        self.assertEqual(first.status, PARTIAL)

        harness2, backend2, graph2 = self._harness(
            [{"core/widget.cpp": "// fully done now\n"}], allow_partial=True,
        )
        harness2.run_node(graph2.find("Widget"))
        self.assertIn("already carries partial work", backend2.prompts[0])
        self.assertIn("needs Helper, unbuilt", backend2.prompts[0])

    def test_noop_check_marks_the_node_done_without_writing_anything(self):
        harness, backend, graph = self._harness(
            ["VERDICT: NOOP\nEVIDENCE: core/existing.h already declares this."],
            noop_check=True,
        )
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, NOOP)
        self.assertEqual(outcome.changed, {})
        self.assertIn("core/existing.h already declares this.", outcome.note)
        self.assertTrue(graph.find("Widget").implemented)
        self.assertIn("Before implementing", backend.prompts[0])

    def test_noop_check_that_disagrees_proceeds_to_implement(self):
        harness, backend, graph = self._harness(
            ["VERDICT: IMPLEMENT", {"core/widget.cpp": "// real work\n"}],
            noop_check=True,
        )
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(outcome.attempts, 1)
        self.assertEqual((self.root / "core" / "widget.cpp").read_text(), "// real work\n")

    def test_noop_check_that_writes_anyway_is_reverted_and_treated_as_implement(self):
        """A checker that disobeys "do not edit files" cannot be trusted, so its
        writes are discarded and the node falls through to a real attempt."""
        harness, backend, graph = self._harness(
            [{"core/sneaky.cpp": "should not survive\n"}, {"core/widget.cpp": "// real work\n"}],
            noop_check=True,
        )
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertFalse((self.root / "core" / "sneaky.cpp").exists())
        self.assertEqual((self.root / "core" / "widget.cpp").read_text(), "// real work\n")

    def test_noop_verdict_is_not_trusted_when_verify_currently_fails(self):
        harness, backend, graph = self._harness(
            ["VERDICT: NOOP\nEVIDENCE: nope, actually not"],
            noop_check=True,
        )
        harness.verifier = Verifier(["false"], self.root)
        outcome = harness.run_node(graph.find("Widget"))
        self.assertNotEqual(outcome.status, NOOP)
        self.assertFalse(graph.find("Widget").implemented)

    def test_confirm_changes_sends_a_blunt_followup_in_the_same_session(self):
        harness, backend, graph = self._harness(
            [{"core/widget.cpp": "// impl\n"}, "yes, done"],
            supports_continue=True, confirm_changes=True,
        )
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(len(backend.prompts), 2)
        self.assertIn("Were the changes made", backend.prompts[1])
        self.assertEqual(backend.continued, [False, True])

    def test_confirm_changes_is_skipped_for_a_backend_that_cannot_continue(self):
        harness, backend, graph = self._harness(
            [{"core/widget.cpp": "// impl\n"}],
            supports_continue=False, confirm_changes=True,
        )
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(len(backend.prompts), 1)

    def test_fix_round_resolves_scope_violation_instead_of_reverting(self):
        harness, backend, graph = self._harness(
            [{"core/widget.cpp": "// ok\n", "keepout/other.cpp": "oops\n"},
             {"keepout/other.cpp": "original\n"}],  # fix round undoes the stray write
            supports_continue=True, fix_rounds=2,
        )
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(outcome.attempts, 1)
        self.assertEqual((self.root / "keepout" / "other.cpp").read_text(), "original\n")
        self.assertEqual((self.root / "core" / "widget.cpp").read_text(), "// ok\n")

    def test_fix_round_resolves_a_failed_gate_instead_of_restarting(self):
        harness, backend, graph = self._harness(
            [{"core/widget.cpp": "// first\n"},
             {"core/widget.cpp": "// first\n", "core/needed.txt": "x\n"}],
            supports_continue=True, fix_rounds=2,
        )
        harness.verifier = Verifier(["test -f core/needed.txt"], self.root)
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(outcome.attempts, 1)
        self.assertTrue((self.root / "core" / "needed.txt").exists())

    def test_fix_round_deleting_state_dir_raises_safety_net_lost(self):
        """str_tsne_rs, 2026-08-30: a fix round ran `rm -rf .harness/
        src/clustering/` because git status showed both as untracked. That
        must stop the walk outright, not be treated as a normal failure."""
        harness, backend, graph = self._harness(
            [{"core/widget.cpp": "// ok\n", "keepout/other.cpp": "oops\n"},
             {".harness": "__RMDIR__"}],
            supports_continue=True, fix_rounds=2,
        )
        with self.assertRaises(SafetyNetLost):
            harness.run_node(graph.find("Widget"))

    def test_fix_rounds_exhausted_falls_back_to_revert_and_retry(self):
        harness, backend, graph = self._harness(
            [{"core/widget.cpp": "// bad attempt\n", "keepout/other.cpp": "still bad\n"},
             {"keepout/other.cpp": "still bad\n"},  # fix round does not actually fix it
             {"core/widget.cpp": "// clean second attempt\n"}],  # attempt 2, no violation
            supports_continue=True, fix_rounds=1,
        )
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(outcome.attempts, 2)
        self.assertEqual((self.root / "keepout" / "other.cpp").read_text(), "original\n")
        self.assertEqual((self.root / "core" / "widget.cpp").read_text(), "// clean second attempt\n")

    def test_strong_model_is_chosen_only_for_the_nominated_nodes(self):
        harness, backend, graph = self._harness([{"core/widget.cpp": "x\n"}])
        strong = ScriptedBackend(self.root, [])
        harness.strong_backend = strong
        harness.strong_kinds = frozenset({"Module"})
        self.assertIs(harness._backend_for(graph.find("Widget")), backend)
        self.assertIs(harness._backend_for(graph.find("core")), strong)

    # ---------------------------------------------------------------- gate
    def _reviewed(self, script, replies, ok=True, **opts):
        harness, backend, graph = self._harness(script, **opts)
        judge = ReviewBackend(replies, ok=ok)
        harness.reviewer = Reviewer(judge, PromptBuilder(graph), self.root)
        return harness, backend, graph, judge

    def test_review_agent_is_dispatched_for_every_node_that_builds(self):
        """The traversal dispatches the reviewer itself; it is not optional."""
        harness, _, graph, judge = self._reviewed([{"core/widget.cpp": "x\n"}],
                                                  ["VERDICT: PASS"])
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(judge.calls, 1)
        self.assertTrue(outcome.reviewed)

    def test_review_rejection_fails_the_node_and_feeds_back_the_findings(self):
        harness, backend, graph, judge = self._reviewed(
            [{"core/widget.cpp": "x\n"}, {"core/widget.cpp": "y\n"}],
            ["VERDICT: REVISE\n- widget duplicates existing.h", "VERDICT: PASS"])
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, DONE)
        self.assertEqual(outcome.attempts, 2)
        self.assertEqual(judge.calls, 2)
        self.assertIn("duplicates existing.h", backend.prompts[1])

    def test_the_build_gate_runs_before_the_reviewer_and_short_circuits_it(self):
        """No point paying a review model to read a diff that does not compile."""
        harness, _, graph, judge = self._reviewed([{"core/widget.cpp": "x\n"}], ["VERDICT: PASS"])
        harness.verifier = Verifier(["test -f core/never_created.cpp"], self.root)
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, FAILED)
        self.assertEqual(judge.calls, 0)
        self.assertTrue(outcome.note.startswith("verify failed"), outcome.note)

    def test_an_unreachable_reviewer_passes_the_node_by_default(self):
        """review.py fails open, so an advisory reviewer never blocks the walk."""
        harness, _, graph, _ = self._reviewed([{"core/widget.cpp": "x\n"}], [], ok=False)
        self.assertEqual(harness.run_node(graph.find("Widget")).status, DONE)

    def test_require_review_turns_an_unreachable_reviewer_into_a_failure(self):
        """Under --require-review, silence from the reviewer is a refusal."""
        harness, _, graph, _ = self._reviewed([{"core/widget.cpp": "x\n"},
                                               {"core/widget.cpp": "y\n"}], [], ok=False,
                                              require_review=True)
        outcome = harness.run_node(graph.find("Widget"))
        self.assertEqual(outcome.status, FAILED)
        self.assertIn("reviewer did not run", outcome.note)
        self.assertFalse(graph.find("Widget").implemented)

    def test_a_broken_tree_stops_the_walk_before_any_node_runs(self):
        harness, backend, graph = self._harness([{"core/widget.cpp": "x\n"}])
        harness.verifier = Verifier(["test -f core/never_created.cpp"], self.root)
        outcomes = harness.run([graph.find("Widget").index])
        self.assertEqual(outcomes, [])
        self.assertEqual(backend.prompts, [])

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
