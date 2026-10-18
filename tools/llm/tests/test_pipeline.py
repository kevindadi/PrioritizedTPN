"""End-to-end tests for prompt generation, measurement, TDG emission, and CLI."""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

_REPO_ROOT = str(Path(__file__).resolve().parents[3])
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from tools.llm.__main__ import main  # noqa: E402
from tools.llm.client import FakeClient, ResponseParseError  # noqa: E402
from tools.llm.pipeline import (  # noqa: E402
    DEFAULT_WCET_METHOD,
    MeasurementError,
    build_prompt,
    emit_tdg,
    generate_model,
    measure_task,
)
from tools.llm.schema import SchemaError, validate_spec  # noqa: E402

TRIVIAL_C = """#include <stdint.h>
int main(void) {
  volatile uint64_t x = 0;
  for (uint64_t i = 0; i < 1000; ++i) {
    x += i;
  }
  return 0;
}
"""

INFINITE_C = """int main(void) {
  volatile unsigned long x = 0;
  for (;;) {
    x += 1;
  }
  return (int)x;
}
"""

BROKEN_C = """int main(void) {
#error intentional compile failure
  return 0;
}
"""


def one_task_spec():
    return {
        "num_cpus": 1,
        "cores_per_cpu": 1,
        "tasks": [
            {
                "name": "A",
                "description": "single root task",
                "period": 0,
                "deadline": 0,
                "priority": 10,
                "allowed_cores": [0],
            }
        ],
    }


def two_task_spec():
    return {
        "num_cpus": 1,
        "cores_per_cpu": 1,
        "tasks": [
            {
                "name": "A",
                "description": "root task",
                "period": 0,
                "deadline": 0,
                "priority": 10,
                "allowed_cores": [0],
            },
            {
                "name": "B",
                "description": "dependent task",
                "period": 0,
                "deadline": 0,
                "priority": 9,
                "allowed_cores": [0],
                "depends_on": ["A"],
            },
        ],
    }


def one_task_model():
    return {"tasks": [{"name": "A", "source": TRIVIAL_C}]}


def two_task_model():
    return {
        "tasks": [
            {"name": "A", "source": TRIVIAL_C},
            {"name": "B", "source": TRIVIAL_C, "depends_on": ["A"]},
        ]
    }


def _write(path, text):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


class PromptAndGenerationTests(unittest.TestCase):
    def test_prompt_is_deterministic_and_contains_contract(self):
        spec = one_task_spec()
        first = build_prompt(spec)
        second = build_prompt(validate_spec(spec))
        self.assertEqual(first, second)
        self.assertIn("worst-case path", first)
        self.assertIn("terminate", first)
        self.assertIn("Return only the JSON object", first)
        self.assertIn('"A"', first)
        self.assertIn('"tasks"', first)

    def test_generate_model_with_fake_client(self):
        client = FakeClient(json.dumps(one_task_model()))
        generated = generate_model(one_task_spec(), client)
        self.assertEqual([task["name"] for task in generated["tasks"]], ["A"])
        self.assertEqual(client.prompts[0], build_prompt(one_task_spec()))

    def test_generate_model_accepts_fenced_response(self):
        client = FakeClient("```json\n" + json.dumps(one_task_model()) + "\n```")
        generated = generate_model(one_task_spec(), client)
        self.assertEqual(generated["tasks"][0]["name"], "A")

    def test_generate_model_rejects_bad_json(self):
        with self.assertRaises(ResponseParseError):
            generate_model(one_task_spec(), FakeClient("not json"))


class TdgEmissionTests(unittest.TestCase):
    REQUIRED_CONFIG = {
        "num_cpus",
        "cores_per_cpu",
        "shared_locks",
        "policy",
        "start",
        "end",
        "periodic",
    }
    REQUIRED_NODE = {
        "type",
        "id",
        "priority",
        "core",
        "time",
        "allowed_cores",
        "locks",
        "wcet_kind",
        "wcet_method",
    }

    def assert_tdg_contract(self, tdg, spec):
        self.assertEqual(set(tdg), {"graph", "configuration", "nodes", "edges"})
        config = tdg["configuration"]
        self.assertEqual(set(config), self.REQUIRED_CONFIG)
        self.assertGreaterEqual(config["num_cpus"], 1)
        self.assertGreaterEqual(config["cores_per_cpu"], 1)
        max_core = config["num_cpus"] - 1

        node_ids = [node["id"] for node in tdg["nodes"]]
        self.assertEqual(len(node_ids), len(set(node_ids)))
        for node in tdg["nodes"]:
            self.assertTrue(self.REQUIRED_NODE.issubset(set(node)), node)
            self.assertEqual(node["type"], "task")
            self.assertIsInstance(node["priority"], int)
            self.assertIsInstance(node["core"], int)
            self.assertGreaterEqual(node["core"], 0)
            self.assertLessEqual(node["core"], max_core)
            self.assertIn(node["core"], node["allowed_cores"])
            self.assertTrue(node["allowed_cores"])
            self.assertEqual(node["wcet_kind"], "measured")
            self.assertEqual(node["wcet_method"], DEFAULT_WCET_METHOD)

            expected_segments = 2 * len(node["locks"]) + 1
            self.assertEqual(len(node["time"]), expected_segments)
            if not node["locks"]:
                self.assertEqual(len(node["time"]), 1)
            for interval in node["time"]:
                self.assertEqual(len(interval), 2)
                self.assertTrue(all(isinstance(value, int) for value in interval))
                self.assertGreaterEqual(interval[0], 0)
                self.assertGreaterEqual(interval[1], interval[0])

        known = set(node_ids)
        for node in tdg["nodes"]:
            for lock in node["locks"]:
                self.assertTrue(
                    lock.startswith(("mutex", "spin")), "bad lock prefix: {}".format(lock)
                )
                self.assertIn(lock, config["shared_locks"])

        start_tasks = set()
        for binding in config["start"]:
            self.assertEqual(set(binding), {"task", "tokens"})
            self.assertIn(binding["task"], known)
            self.assertGreaterEqual(binding["tokens"], 0)
            start_tasks.add(binding["task"])

        for name in config["end"]:
            self.assertIn(name, known)
        for binding in config["periodic"]:
            self.assertEqual(set(binding), {"task", "period"})
            self.assertIn(binding["task"], known)
            self.assertGreater(binding["period"], 0)

        for edge in tdg["edges"]:
            self.assertTrue({"source", "target"}.issubset(set(edge)), edge)
            self.assertIn(edge["source"], known)
            self.assertIn(edge["target"], known)
            self.assertIsInstance(edge.get("label", ""), str)

    def test_emit_tdg_with_two_tasks(self):
        spec = two_task_spec()
        measured = {"A": {"lo": 100, "hi": 120}, "B": {"lo": 50, "hi": 60}}
        tdg = emit_tdg(spec, two_task_model(), measured)
        self.assert_tdg_contract(tdg, spec)

        self.assertEqual(tdg["graph"]["name"], "LLMGeneratedTaskGraph")
        self.assertEqual(tdg["configuration"]["shared_locks"], [])
        self.assertEqual(tdg["configuration"]["start"], [{"task": "A", "tokens": 1}])
        self.assertEqual(tdg["configuration"]["end"], ["B"])
        self.assertEqual(tdg["configuration"]["periodic"], [])
        self.assertEqual(
            tdg["edges"], [{"source": "A", "target": "B", "label": "0,0"}]
        )

        nodes = dict((node["id"], node) for node in tdg["nodes"])
        self.assertEqual(nodes["A"]["time"], [[100, 120]])
        self.assertEqual(nodes["B"]["time"], [[50, 60]])
        self.assertEqual(nodes["A"]["core"], 0)
        self.assertEqual(nodes["A"]["deadline"], spec["tasks"][0]["deadline"])

    def test_emit_tdg_locked_task_uses_2n_plus_1_segments(self):
        spec = two_task_spec()
        spec["num_cpus"] = 2
        spec["tasks"][1]["allowed_cores"] = [0, 1]
        spec["tasks"][1]["locks"] = ["mutex0"]
        model = two_task_model()  # declarations fall back to the spec
        # Pre/CS/Post measurements, one interval per TDG segment.
        measured = {
            "A": {"lo": 100, "hi": 120},
            "B": [[20, 25], [25, 30], [5, 5]],
        }
        tdg = emit_tdg(spec, model, measured)
        self.assert_tdg_contract(tdg, spec)

        nodes = dict((node["id"], node) for node in tdg["nodes"])
        self.assertEqual(nodes["B"]["locks"], ["mutex0"])
        self.assertEqual(nodes["B"]["time"], [[20, 25], [25, 30], [5, 5]])
        self.assertEqual(tdg["configuration"]["shared_locks"], ["mutex0"])

    def test_emit_tdg_rejects_total_interval_for_locked_task(self):
        spec = two_task_spec()
        spec["num_cpus"] = 2
        spec["tasks"][1]["allowed_cores"] = [0, 1]
        spec["tasks"][1]["locks"] = ["mutex0"]
        model = two_task_model()
        measured = {"A": {"lo": 100, "hi": 120}, "B": {"lo": 50, "hi": 60}}
        with self.assertRaises(SchemaError):
            emit_tdg(spec, model, measured)

    def test_emit_tdg_rejects_wrong_segment_count_for_locked_task(self):
        spec = two_task_spec()
        spec["num_cpus"] = 2
        spec["tasks"][1]["allowed_cores"] = [0, 1]
        spec["tasks"][1]["locks"] = ["mutex0"]
        model = two_task_model()
        measured = {
            "A": {"lo": 100, "hi": 120},
            "B": [[20, 25], [25, 30]],
        }
        with self.assertRaises(SchemaError):
            emit_tdg(spec, model, measured)

    def test_generated_declarations_replace_spec_hints(self):
        spec = two_task_spec()
        spec["num_cpus"] = 2
        spec["tasks"][1]["allowed_cores"] = [0, 1]
        spec["tasks"][1]["locks"] = ["mutex0"]
        model = {
            "tasks": [
                {"name": "A", "source": TRIVIAL_C},
                {
                    "name": "B",
                    "source": TRIVIAL_C,
                    "depends_on": [],
                    "locks": [],
                    "allowed_cores": [1],
                },
            ]
        }
        measured = {"A": {"lo": 10, "hi": 12}, "B": {"lo": 20, "hi": 25}}
        tdg = emit_tdg(spec, model, measured)
        self.assert_tdg_contract(tdg, spec)

        nodes = dict((node["id"], node) for node in tdg["nodes"])
        self.assertEqual(nodes["B"]["core"], 1)
        self.assertEqual(nodes["B"]["allowed_cores"], [1])
        self.assertEqual(nodes["B"]["locks"], [])
        self.assertEqual(nodes["B"]["time"], [[20, 25]])
        self.assertEqual(tdg["edges"], [])
        self.assertEqual(tdg["configuration"]["shared_locks"], [])
        self.assertEqual(
            tdg["configuration"]["start"],
            [{"task": "A", "tokens": 1}, {"task": "B", "tokens": 1}],
        )

    def test_explicit_activation_and_release_bindings(self):
        spec = one_task_spec()
        spec["tasks"][0]["period"] = 100
        spec["start"] = [{"task": "A", "tokens": 2}]
        spec["end"] = ["A"]
        spec["periodic"] = [{"task": "A", "period": 100}]
        tdg = emit_tdg(
            spec,
            one_task_model(),
            {"A": {"lo": 5, "hi": 9}},
        )
        self.assert_tdg_contract(tdg, spec)
        self.assertEqual(tdg["configuration"]["start"], [{"task": "A", "tokens": 2}])
        self.assertEqual(tdg["configuration"]["periodic"], [{"task": "A", "period": 100}])

    def test_default_periodic_release_from_task_period(self):
        spec = one_task_spec()
        spec["tasks"][0]["period"] = 250
        tdg = emit_tdg(spec, one_task_model(), {"A": [1, 2]})
        self.assertEqual(tdg["configuration"]["periodic"], [{"task": "A", "period": 250}])
        self.assertEqual(tdg["configuration"]["start"], [])

    def test_measured_interval_validation(self):
        spec = two_task_spec()
        model = two_task_model()
        with self.assertRaises(SchemaError):
            emit_tdg(spec, model, {"A": {"lo": 1, "hi": 2}})
        with self.assertRaises(SchemaError):
            emit_tdg(
                spec,
                model,
                {"A": {"lo": 1, "hi": 2}, "B": {"lo": 1, "hi": 2}, "C": [0, 1]},
            )
        with self.assertRaises(SchemaError):
            emit_tdg(spec, model, {"A": {"lo": 5, "hi": 4}, "B": {"lo": 1, "hi": 2}})
        with self.assertRaises(SchemaError):
            emit_tdg(spec, model, {"A": {"lo": True, "hi": 4}, "B": {"lo": 1, "hi": 2}})


class MeasurementTests(unittest.TestCase):
    def test_execution_is_disabled_by_default(self):
        with self.assertRaises(MeasurementError) as raised:
            measure_task(TRIVIAL_C)
        self.assertEqual(raised.exception.stage, "disabled")

    def test_missing_compiler_is_a_structured_compile_error(self):
        with self.assertRaises(MeasurementError) as raised:
            measure_task(
                TRIVIAL_C,
                runs=1,
                timeout_s=1,
                compiler="ptpn-no-such-compiler",
                allow_execute=True,
            )
        self.assertEqual(raised.exception.stage, "compile")
        self.assertIn("could not start", raised.exception.message)

    def test_invalid_measurement_arguments(self):
        with self.assertRaises(MeasurementError):
            measure_task(TRIVIAL_C, runs=0, allow_execute=True)
        with self.assertRaises(MeasurementError):
            measure_task(TRIVIAL_C, timeout_s=0, allow_execute=True)

    @unittest.skipUnless(shutil.which("cc"), "no C compiler available")
    def test_compile_and_run(self):
        result = measure_task(TRIVIAL_C, runs=3, timeout_s=5, allow_execute=True)
        self.assertEqual(set(result), {"lo", "hi"})
        self.assertIsInstance(result["lo"], int)
        self.assertIsInstance(result["hi"], int)
        self.assertGreaterEqual(result["lo"], 0)
        self.assertGreaterEqual(result["hi"], result["lo"])

    @unittest.skipUnless(shutil.which("cc"), "no C compiler available")
    def test_compile_error_carries_diagnostics(self):
        with self.assertRaises(MeasurementError) as raised:
            measure_task(BROKEN_C, runs=1, timeout_s=2, allow_execute=True)
        error = raised.exception
        self.assertEqual(error.stage, "compile")
        self.assertNotEqual(error.returncode, 0)
        self.assertIn("error", error.stderr.lower())
        payload = error.to_dict()
        self.assertEqual(payload["stage"], "compile")
        self.assertIn("stderr", payload)

    @unittest.skipUnless(shutil.which("cc"), "no C compiler available")
    def test_wall_clock_timeout(self):
        with self.assertRaises(MeasurementError) as raised:
            measure_task(INFINITE_C, runs=1, timeout_s=0.5, allow_execute=True)
        self.assertEqual(raised.exception.stage, "run")
        self.assertTrue(raised.exception.timed_out)


class CliTests(unittest.TestCase):
    def _make_inputs(self, directory, execute_source=TRIVIAL_C):
        spec_path = os.path.join(directory, "spec.json")
        response_path = os.path.join(directory, "response.txt")
        out_path = os.path.join(directory, "model.json")
        _write(spec_path, json.dumps(one_task_spec()))
        _write(
            response_path,
            "```json\n" + json.dumps({"tasks": [{"name": "A", "source": execute_source}]}) + "\n```",
        )
        return spec_path, response_path, out_path

    def test_offline_replay_reports_needs_measurement(self):
        with tempfile.TemporaryDirectory() as directory:
            spec_path, response_path, out_path = self._make_inputs(directory)
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = main(
                    [
                        "generate",
                        "--spec",
                        spec_path,
                        "--response-file",
                        response_path,
                        "--out",
                        out_path,
                    ]
                )
            self.assertEqual(code, 0)
            self.assertTrue(os.path.isfile(out_path))
            with open(out_path, "r", encoding="utf-8") as handle:
                saved = json.load(handle)
            self.assertEqual(saved["tasks"][0]["name"], "A")
            report = json.loads(stdout.getvalue())
            self.assertEqual(report["status"], "needs_measurement")
            self.assertEqual(report["tasks"], ["A"])
            default_tdg = str(Path(out_path).with_suffix(".tdg.json"))
            self.assertFalse(os.path.exists(default_tdg))

    def test_refuses_tdg_without_execute(self):
        with tempfile.TemporaryDirectory() as directory:
            spec_path, response_path, out_path = self._make_inputs(directory)
            tdg_path = os.path.join(directory, "graph.json")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = main(
                    [
                        "generate",
                        "--spec",
                        spec_path,
                        "--response-file",
                        response_path,
                        "--out",
                        out_path,
                        "--tdg-out",
                        tdg_path,
                    ]
                )
            self.assertEqual(code, 2)
            self.assertIn("refusing", stderr.getvalue())
            self.assertFalse(os.path.exists(tdg_path))

    def test_measured_json_emits_tdg_without_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            spec_path, response_path, out_path = self._make_inputs(directory)
            measured_path = os.path.join(directory, "measured.json")
            tdg_path = os.path.join(directory, "graph.json")
            _write(measured_path, json.dumps({"A": {"lo": 3, "hi": 5}}))
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = main(
                    [
                        "generate",
                        "--spec",
                        spec_path,
                        "--response-file",
                        response_path,
                        "--out",
                        out_path,
                        "--measured-json",
                        measured_path,
                        "--tdg-out",
                        tdg_path,
                    ]
                )
            self.assertEqual(code, 0)
            with open(tdg_path, "r", encoding="utf-8") as handle:
                tdg = json.load(handle)
            self.assertEqual(tdg["nodes"][0]["time"], [[3, 5]])
            self.assertEqual(tdg["nodes"][0]["wcet_kind"], "measured")
            report = json.loads(stdout.getvalue())
            self.assertEqual(report["status"], "ok")
            self.assertEqual(report["measured"], {"A": {"lo": 3, "hi": 5}})

    def test_dry_run_prints_prompt_without_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            spec_path, _, out_path = self._make_inputs(directory)
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = main(
                    ["generate", "--spec", spec_path, "--out", out_path, "--dry-run"]
                )
            self.assertEqual(code, 0)
            self.assertIn("worst-case path", stdout.getvalue())
            self.assertFalse(os.path.exists(out_path))

    @unittest.skipUnless(shutil.which("cc"), "no C compiler available")
    def test_offline_execute_emits_measured_tdg(self):
        with tempfile.TemporaryDirectory() as directory:
            spec_path, response_path, out_path = self._make_inputs(directory)
            tdg_path = os.path.join(directory, "graph.json")
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = main(
                    [
                        "generate",
                        "--spec",
                        spec_path,
                        "--response-file",
                        response_path,
                        "--out",
                        out_path,
                        "--execute",
                        "--runs",
                        "2",
                        "--timeout",
                        "5",
                        "--tdg-out",
                        tdg_path,
                    ]
                )
            self.assertEqual(code, 0)
            with open(tdg_path, "r", encoding="utf-8") as handle:
                tdg = json.load(handle)
            self.assertEqual(tdg["nodes"][0]["wcet_kind"], "measured")
            self.assertEqual(tdg["nodes"][0]["wcet_method"], DEFAULT_WCET_METHOD)
            report = json.loads(stdout.getvalue())
            self.assertEqual(report["status"], "ok")
            self.assertIn("A", report["measured"])


if __name__ == "__main__":
    unittest.main()
