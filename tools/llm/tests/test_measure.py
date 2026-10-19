"""Tests for the RTSL measurement loop (envelope, injection, end to end)."""

import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_REPO_ROOT = str(Path(__file__).resolve().parents[3])
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from tools.llm.__main__ import main  # noqa: E402
from tools.llm.measure import (  # noqa: E402
    DEFAULT_RUNS,
    DEFAULT_TIMEOUT_S,
    EnvelopeError,
    InjectionError,
    MeasureError,
    apply_injections,
    build_measurement_spec,
    collect_replacements,
    ensure_ptpn_bin,
    escape_rtsl_string,
    format_artifact,
    format_time_attribute,
    format_time_attribute_template,
    load_task_descriptions,
    observed_ns_to_ms,
    parse_envelope,
    plan_segments,
    render_diagnostics,
    resolve_ptpn_bin,
    run_check,
    segment_role,
    validate_pending_measurements,
)
from tools.llm.pipeline import build_prompt  # noqa: E402
from tools.llm.schema import validate_spec  # noqa: E402

TRIVIAL_C = """#include <stdint.h>
int main(void) {
  volatile uint64_t x = 0;
  for (uint64_t i = 0; i < 1000; ++i) {
    x += i;
  }
  return 0;
}
"""

LOCKED_RTSL = """system demo {
  cpus 1
  cores_per_cpu 1
  policy fixed

  task fusion: aperiodic {
    core 0
    locks mutex1
    time measured by llm { runs 2, timeout 5 }
  }

  start fusion
  end fusion
}
"""

SINGLE_RTSL = """system demo {
  cpus 1
  task post: normal {
    core 0
    time measured by llm { runs 2, timeout 5 }
  }

  start post
  end post
}
"""

INVALID_RTSL = """system demo {
  cpus 1
  task a: normal {
    core 0
    bogus
  }
}
"""


def _ptpn_candidates():
    candidates = []
    explicit = os.environ.get("PTPN_TEST_BIN")
    if explicit:
        candidates.append(Path(explicit).expanduser())
    candidates.append(Path(_REPO_ROOT) / ("ptpn.exe" if os.name == "nt" else "ptpn"))
    candidates.append(Path("/tmp/phase2b-off/ptpn"))
    # The project build script also writes this marker.
    marker = Path(_REPO_ROOT) / ".ptpn-build-dir"
    if marker.is_file():
        line = marker.read_text(encoding="utf-8").strip()
        if line:
            build = Path(line).expanduser()
            if not build.is_absolute():
                build = Path(_REPO_ROOT) / build
            candidates.append(build / ("ptpn.exe" if os.name == "nt" else "ptpn"))
    return candidates


def _find_ptpn():
    for candidate in _ptpn_candidates():
        if candidate.is_file():
            return str(candidate)
    return None


PTPN_BIN = _find_ptpn()
PTPN_SKIP = (
    "ptpn binary unavailable; set PTPN_TEST_BIN, build build/ptpn, "
    "or keep /tmp/phase2b-off/ptpn"
)
CC_AVAILABLE = shutil.which("cc") is not None


def _write(path, text):
    Path(path).write_text(text, encoding="utf-8")


def _envelope(pending=None, ok=True, exit_code=0, diagnostics=None):
    value = {
        "protocol": "ptpn-diagnostics",
        "version": 1,
        "ok": ok,
        "exit_code": exit_code,
        "diagnostics": diagnostics if diagnostics is not None else [],
        "truncated_diagnostics": False,
    }
    if ok:
        value["result"] = {"pending_measurements": pending or []}
    return value


def _pending(task="a", expected_segments=3, runs=2, timeout=5):
    return {
        "task": task,
        "expected_segments": expected_segments,
        "runs": runs,
        "timeout": timeout,
        "inject": {
            "file": "model.rtsl",
            "byte_start": 10,
            "byte_end": 50,
            "line": 4,
            "column": 5,
        },
    }


class EnvelopeTests(unittest.TestCase):
    def test_valid_envelope_is_normalized(self):
        parsed = parse_envelope(json.dumps(_envelope([_pending()])))
        self.assertEqual(parsed["protocol"], "ptpn-diagnostics")
        self.assertTrue(parsed["ok"])
        self.assertEqual(parsed["exit_code"], 0)
        self.assertEqual(len(parsed["pending_measurements"]), 1)
        self.assertFalse(parsed["truncated_diagnostics"])

    def test_failure_envelope_needs_no_result(self):
        parsed = parse_envelope(json.dumps(_envelope(ok=False, exit_code=3)))
        self.assertFalse(parsed["ok"])
        self.assertEqual(parsed["pending_measurements"], [])

    def test_missing_json_is_rejected(self):
        for text in ("", "   ", "not json", "<html></html>"):
            with self.assertRaises(EnvelopeError):
                parse_envelope(text)

    def test_non_object_json_is_rejected(self):
        with self.assertRaises(EnvelopeError):
            parse_envelope("[]")
        with self.assertRaises(EnvelopeError):
            parse_envelope('"text"')

    def test_duplicate_keys_are_rejected(self):
        with self.assertRaises(EnvelopeError):
            parse_envelope('{"protocol": "ptpn-diagnostics", "version": 1, "version": 1}')

    def test_wrong_protocol_is_rejected(self):
        value = _envelope()
        value["protocol"] = "something-else"
        with self.assertRaises(EnvelopeError):
            parse_envelope(json.dumps(value))

    def test_missing_or_invalid_structural_fields_are_rejected(self):
        without_ok = _envelope()
        del without_ok["ok"]
        with self.assertRaises(EnvelopeError):
            parse_envelope(json.dumps(without_ok))

        without_exit = _envelope()
        del without_exit["exit_code"]
        with self.assertRaises(EnvelopeError):
            parse_envelope(json.dumps(without_exit))

        bad_version = _envelope()
        bad_version["version"] = "1"
        with self.assertRaises(EnvelopeError):
            parse_envelope(json.dumps(bad_version))

        bad_pending = _envelope()
        bad_pending["result"] = {"pending_measurements": {}}
        with self.assertRaises(EnvelopeError):
            parse_envelope(json.dumps(bad_pending))

    def test_pending_defaults_match_spec(self):
        raw = _pending()
        del raw["runs"]
        del raw["timeout"]
        pending = validate_pending_measurements([raw])
        self.assertEqual(pending[0]["runs"], DEFAULT_RUNS)
        self.assertEqual(pending[0]["timeout"], DEFAULT_TIMEOUT_S)

    def test_pending_without_inject_is_rejected(self):
        raw = _pending()
        del raw["inject"]
        with self.assertRaises(EnvelopeError):
            validate_pending_measurements([raw])

    def test_pending_with_invalid_span_is_rejected(self):
        for start, end in ((-1, 5), (10, 10), (10, 9), ("10", 20), (None, 20)):
            raw = _pending()
            raw["inject"]["byte_start"] = start
            raw["inject"]["byte_end"] = end
            with self.assertRaises(EnvelopeError):
                validate_pending_measurements([raw])

    def test_pending_with_nonpositive_runs_or_timeout_is_rejected(self):
        for field in ("runs", "timeout"):
            for value in (0, -1, True, "2"):
                raw = _pending()
                raw[field] = value
                with self.assertRaises(EnvelopeError):
                    validate_pending_measurements([raw])

    def test_duplicate_pending_task_is_rejected(self):
        with self.assertRaises(EnvelopeError):
            validate_pending_measurements([_pending(), _pending()])

    def test_render_diagnostics_is_human_readable(self):
        envelope = _envelope(
            ok=False,
            exit_code=2,
            diagnostics=[
                {
                    "code": "E-SYN-102",
                    "category": "syntax",
                    "severity": "error",
                    "location": {
                        "file": "model.rtsl",
                        "line": 6,
                        "column": 5,
                        "span": 5,
                    },
                    "subject": "task:a",
                    "message": "unexpected token 'bogus'",
                    "fix": "rewrite this line",
                }
            ],
        )
        text = render_diagnostics(envelope)
        self.assertIn("exit 2", text)
        self.assertIn("E-SYN-102", text)
        self.assertIn("model.rtsl:6:5", text)
        self.assertIn("unexpected token", text)
        self.assertIn("fix: rewrite this line", text)


class BinaryResolutionTests(unittest.TestCase):
    def test_default_is_build_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            resolved = resolve_ptpn_bin(None, {}, repo_root=root)
            self.assertEqual(
                resolved, (root / "build" / ("ptpn.exe" if os.name == "nt" else "ptpn")).resolve()
            )

    def test_marker_file_overrides_default(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".ptpn-build-dir").write_text("build-asan\n", encoding="utf-8")
            resolved = resolve_ptpn_bin(None, {}, repo_root=root)
            self.assertEqual(
                resolved,
                (root / "build-asan" / ("ptpn.exe" if os.name == "nt" else "ptpn")).resolve(),
            )

    def test_environment_overrides_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".ptpn-build-dir").write_text("marker-build\n", encoding="utf-8")
            resolved = resolve_ptpn_bin(
                None, {"PTPN_BUILD_DIR": "env-build"}, repo_root=root
            )
            self.assertEqual(
                resolved,
                (root / "env-build" / ("ptpn.exe" if os.name == "nt" else "ptpn")).resolve(),
            )

    def test_explicit_path_wins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(
                resolve_ptpn_bin("custom/ptpn", {"PTPN_BUILD_DIR": "ignored"}, repo_root=root),
                (root / "custom" / "ptpn").resolve(),
            )
            absolute = (root / "absolute-ptpn").resolve()
            self.assertEqual(
                resolve_ptpn_bin(str(absolute), {"PTPN_BUILD_DIR": "ignored"}, repo_root=root),
                absolute,
            )

    def test_missing_binary_reports_a_build_hint(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(MeasureError) as raised:
                ensure_ptpn_bin(Path(directory) / "missing-ptpn")
            message = str(raised.exception)
            self.assertIn("not found", message)
            self.assertIn("./build.py", message)


class ReplacementFormattingTests(unittest.TestCase):
    def test_single_segment_golden(self):
        self.assertEqual(
            format_time_attribute([(2, 3)], 20, 5, "sha256:abc cc:cc -O2 task.c -lm"),
            'time [2, 3] measured by llm { runs 20, timeout 5, '
            'artifact "sha256:abc cc:cc -O2 task.c -lm" }',
        )

    def test_multi_segment_golden(self):
        self.assertEqual(
            format_time_attribute(
                [(1, 2), (3, 4), (1, 2)],
                20,
                5,
                "sha256:a,b,c cc:x ; y ; z",
            ),
            "time [ [1, 2], [3, 4], [1, 2] ] measured by llm { runs 20, "
            'timeout 5, artifact "sha256:a,b,c cc:x ; y ; z" }',
        )

    def test_artifact_string_is_escaped_for_rtsl(self):
        replacement = format_time_attribute([(1, 1)], 1, 1, 'say "hi"\\there')
        self.assertIn('artifact "say \\"hi\\"\\\\there"', replacement)
        self.assertEqual(escape_rtsl_string("a\nb\tc"), "a\\nb c")

    def test_invalid_segments_are_rejected(self):
        for segments in ([], [(3, 2)], [(-1, 2)], [(1,)], [("1", 2)]):
            with self.assertRaises(ValueError):
                format_time_attribute(segments, 1, 1, "artifact")
        with self.assertRaises(ValueError):
            format_time_attribute([(1, 1)], 0, 1, "artifact")
        with self.assertRaises(ValueError):
            format_time_attribute([(1, 1)], 1, 0, "artifact")
        with self.assertRaises(ValueError):
            format_time_attribute([(1, 1)], 1, 1, "")

    def test_dry_run_template(self):
        single = format_time_attribute_template(1, 20, 5)
        multi = format_time_attribute_template(3, 20, 5)
        self.assertIn("time [<min>, <max>] measured by llm", single)
        self.assertIn("time [ [<min>, <max>], [<min>, <max>], [<min>, <max>] ]", multi)
        self.assertIn("runs 20, timeout 5", single)


class ArtifactTests(unittest.TestCase):
    def test_single_segment_artifact(self):
        metadata = {
            "binary_sha256": "a" * 64,
            "compile_command": [
                "cc",
                "-O2",
                "-std=c11",
                "-o",
                "task_bin",
                "task.c",
                "-lm",
            ],
        }
        self.assertEqual(
            format_artifact([metadata]),
            "sha256:{} cc:cc -O2 -std=c11 -o task_bin task.c -lm".format("a" * 64),
        )

    def test_multi_segment_artifact_lists_each_binary_and_command(self):
        metadata = [
            {"binary_sha256": "a" * 64, "compile_command": ["cc", "-O2", "one.c"]},
            {"binary_sha256": "b" * 64, "compile_command": ["cc", "-O2", "two.c"]},
        ]
        artifact = format_artifact(metadata)
        self.assertEqual(
            artifact,
            "sha256:{},{} cc:cc -O2 one.c ; cc -O2 two.c".format("a" * 64, "b" * 64),
        )
        self.assertTrue(artifact.startswith("sha256:"))
        self.assertIn(" cc:", artifact)

    def test_invalid_metadata_is_rejected(self):
        with self.assertRaises(ValueError):
            format_artifact([])
        with self.assertRaises(ValueError):
            format_artifact([{"binary_sha256": "abc", "compile_command": ["cc"]}])
        with self.assertRaises(ValueError):
            format_artifact([{"binary_sha256": "a" * 64, "compile_command": []}])


class UnitConversionTests(unittest.TestCase):
    def test_nanoseconds_round_up_to_milliseconds(self):
        self.assertEqual(observed_ns_to_ms(0), 0)
        self.assertEqual(observed_ns_to_ms(1), 1)
        self.assertEqual(observed_ns_to_ms(999_999), 1)
        self.assertEqual(observed_ns_to_ms(1_000_000), 1)
        self.assertEqual(observed_ns_to_ms(1_000_001), 2)
        self.assertEqual(observed_ns_to_ms(2_683_000), 3)
        self.assertEqual(observed_ns_to_ms(3_228_999), 4)

    def test_non_integer_input_is_rejected(self):
        for value in (1.5, True, "1"):
            with self.assertRaises(MeasureError):
                observed_ns_to_ms(value)


class PlanTests(unittest.TestCase):
    def test_single_segment_keeps_the_task_name(self):
        plan = plan_segments(
            [{"task": "post", "expected_segments": 1, "runs": 2, "timeout": 5}],
            {"post": "consume a sample"},
        )
        self.assertEqual(len(plan), 1)
        self.assertEqual(plan[0]["proposal_name"], "post")
        self.assertIn("consume a sample", plan[0]["description"])

    def test_locked_task_yields_one_proposal_per_segment(self):
        plan = plan_segments(
            [_pending(task="fusion", expected_segments=3)],
            {"fusion": "process one sample"},
        )
        self.assertEqual(
            [segment["proposal_name"] for segment in plan],
            ["fusion::seg0", "fusion::seg1", "fusion::seg2"],
        )
        self.assertEqual(
            [segment["role"] for segment in plan],
            [
                "code executed before acquiring lock 1 (no locks held)",
                "the critical section executed while holding all 1 lock(s)",
                "code executed after releasing lock 1 (no locks held)",
            ],
        )
        for segment in plan:
            self.assertIn("process one sample", segment["description"])
            self.assertIn("segment {} of 3".format(segment["segment"] + 1), segment["description"])

    def test_two_lock_segment_roles_follow_docs_rule(self):
        self.assertEqual(
            [segment_role(2, index) for index in range(5)],
            [
                "code executed before acquiring lock 1 (no locks held)",
                "code executed after acquiring lock 1 and before acquiring lock 2 "
                "(holding 1 lock(s))",
                "the critical section executed while holding all 2 lock(s)",
                "code executed after releasing lock 2 (holding 1 lock(s))",
                "code executed after releasing lock 1 (no locks held)",
            ],
        )

    def test_missing_description_is_an_error(self):
        with self.assertRaises(MeasureError) as raised:
            plan_segments([_pending(task="fusion")], {})
        self.assertIn("--tasks-json", str(raised.exception))

    def test_measurement_spec_is_valid_and_deterministic(self):
        plan = plan_segments(
            [_pending(task="fusion", expected_segments=3)],
            {"fusion": "process one sample"},
        )
        spec = build_measurement_spec(plan)
        validate_spec(spec)
        self.assertEqual(build_prompt(spec), build_prompt(spec))
        self.assertIn("fusion::seg0", build_prompt(spec))
        self.assertIn("fusion::seg2", build_prompt(spec))

    def test_tasks_json_loading(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tasks.json"
            path.write_text(
                json.dumps({"fusion": " process ", "post": "consume"}), encoding="utf-8"
            )
            self.assertEqual(
                load_task_descriptions(str(path)),
                {"fusion": "process", "post": "consume"},
            )

            path.write_text(json.dumps(["not", "an", "object"]), encoding="utf-8")
            with self.assertRaises(MeasureError):
                load_task_descriptions(str(path))

            path.write_text(json.dumps({"fusion": ""}), encoding="utf-8")
            with self.assertRaises(MeasureError):
                load_task_descriptions(str(path))

            path.write_text("{not json", encoding="utf-8")
            with self.assertRaises(MeasureError):
                load_task_descriptions(str(path))


class InjectionTests(unittest.TestCase):
    def _data_and_pending(self):
        data = (
            b"system demo {\n"
            b"  task a {\n"
            b"    time measured by llm { runs 2, timeout 5 }\n"
            b"  }\n"
            b"}\n"
        )
        start = data.index(b"time")
        end = data.index(b"}", start) + 1
        pending = [
            {
                "task": "a",
                "expected_segments": 1,
                "runs": 2,
                "timeout": 5,
                "inject": {
                    "file": "model.rtsl",
                    "byte_start": start,
                    "byte_end": end,
                    "line": 3,
                    "column": 5,
                },
            }
        ]
        return data, pending

    def test_slice_replace_round_trip(self):
        data, pending = self._data_and_pending()
        replacement = format_time_attribute(
            [(1, 2)], 2, 5, "sha256:{} cc:cc -O2 task.c".format("a" * 64)
        )
        items = collect_replacements(data, pending, {"a": replacement})
        result = apply_injections(data, items)
        self.assertIn(replacement.encode("utf-8"), result)
        self.assertNotIn(b"time measured by llm", result)
        self.assertTrue(result.startswith(b"system demo {"))

    def test_stale_span_aborts_without_a_partial_buffer(self):
        data, pending = self._data_and_pending()
        items = collect_replacements(data, pending, {"a": "time [1, 2] measured by llm { runs 2, timeout 5 }"})
        mutated = data.replace(b"runs 2", b"runs 3")
        with self.assertRaises(InjectionError) as raised:
            apply_injections(mutated, items)
        self.assertIn("aborting", str(raised.exception))
        self.assertNotIn(b"time [1, 2]", mutated)

    def test_span_must_cover_a_measured_time_attribute(self):
        data = b"period 10 measured by llm\n"
        pending = [
            {
                "task": "a",
                "expected_segments": 1,
                "runs": 2,
                "timeout": 5,
                "inject": {
                    "file": "model.rtsl",
                    "byte_start": 0,
                    "byte_end": len(data),
                    "line": 1,
                    "column": 1,
                },
            }
        ]
        with self.assertRaises(InjectionError):
            collect_replacements(data, pending, {"a": "replacement"})

    def test_out_of_range_span_is_rejected(self):
        data, pending = self._data_and_pending()
        pending[0]["inject"]["byte_end"] = len(data) + 10
        with self.assertRaises(InjectionError):
            collect_replacements(data, pending, {"a": "replacement"})


@unittest.skipUnless(PTPN_BIN, PTPN_SKIP)
class RealBinaryTests(unittest.TestCase):
    def _check(self, path):
        code, stdout, stderr = run_check(Path(PTPN_BIN), Path(path))
        return code, parse_envelope(stdout), stderr

    def test_check_envelope_contains_inject_span(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.rtsl"
            _write(model, LOCKED_RTSL)
            code, envelope, _ = self._check(model)
            self.assertEqual(code, 0)
            self.assertTrue(envelope["ok"])
            pending = validate_pending_measurements(envelope["pending_measurements"])
            self.assertEqual(len(pending), 1)
            entry = pending[0]
            self.assertEqual(entry["task"], "fusion")
            self.assertEqual(entry["expected_segments"], 3)
            self.assertEqual(entry["runs"], 2)
            self.assertEqual(entry["timeout"], 5)
            data = model.read_bytes()
            span = data[
                entry["inject"]["byte_start"] : entry["inject"]["byte_end"]
            ]
            self.assertEqual(
                span, b"time measured by llm { runs 2, timeout 5 }"
            )

    def test_inject_span_round_trip_against_real_check(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.rtsl"
            _write(model, LOCKED_RTSL)
            code, envelope, _ = self._check(model)
            self.assertEqual(code, 0)
            pending = validate_pending_measurements(envelope["pending_measurements"])
            data = model.read_bytes()
            replacement = format_time_attribute(
                [(1, 2), (2, 3), (1, 1)],
                2,
                5,
                "sha256:{} cc:cc -O2 -o task_bin task.c -lm".format("b" * 64),
            )
            items = collect_replacements(data, pending, {"fusion": replacement})
            model.write_bytes(apply_injections(data, items))

            code, envelope, _ = self._check(model)
            self.assertEqual(code, 0)
            self.assertTrue(envelope["ok"])
            self.assertEqual(envelope["pending_measurements"], [])
            self.assertIn(replacement.encode("utf-8"), model.read_bytes())

    def test_check_failure_prints_diagnostics_and_leaves_file_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.rtsl"
            _write(model, INVALID_RTSL)
            before = model.read_bytes()

            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                code = main(["measure", "--rtsl", str(model), "--ptpn", PTPN_BIN])
            self.assertEqual(code, 2)
            self.assertEqual(model.read_bytes(), before)
            self.assertIn("ptpn check failed", stderr.getvalue())
            self.assertIn("E-SYN-102", stderr.getvalue())
            self.assertEqual(stdout.getvalue(), "")

    def test_dry_run_reports_plan_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.rtsl"
            tasks = Path(directory) / "tasks.json"
            _write(model, LOCKED_RTSL)
            _write(tasks, json.dumps({"fusion": "process one sample"}))
            before = model.read_bytes()

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = main(
                    [
                        "measure",
                        "--rtsl",
                        str(model),
                        "--tasks-json",
                        str(tasks),
                        "--ptpn",
                        PTPN_BIN,
                    ]
                )
            self.assertEqual(code, 0)
            self.assertEqual(model.read_bytes(), before)
            report = json.loads(stdout.getvalue())
            self.assertEqual(report["status"], "dry_run")
            self.assertEqual(report["pending_measurements"][0]["task"], "fusion")
            self.assertIn("measured by llm", report["pending_measurements"][0]["replacement_template"])
            self.assertIn("fusion::seg0", report["prompt"])

    def test_pending_task_without_description_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.rtsl"
            tasks = Path(directory) / "tasks.json"
            _write(model, LOCKED_RTSL)
            _write(tasks, json.dumps({"other": "unrelated"}))
            before = model.read_bytes()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = main(
                    [
                        "measure",
                        "--rtsl",
                        str(model),
                        "--tasks-json",
                        str(tasks),
                        "--ptpn",
                        PTPN_BIN,
                    ]
                )
            self.assertEqual(code, 2)
            self.assertIn("no description", stderr.getvalue())
            self.assertEqual(model.read_bytes(), before)

    @unittest.skipUnless(CC_AVAILABLE, "no C compiler available")
    def test_execute_injects_in_place_and_rechecks_cleanly(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.rtsl"
            tasks = Path(directory) / "tasks.json"
            response = Path(directory) / "response.json"
            _write(model, LOCKED_RTSL)
            _write(tasks, json.dumps({"fusion": "process one sample"}))
            _write(
                response,
                json.dumps(
                    {
                        "tasks": [
                            {"name": "fusion::seg0", "source": TRIVIAL_C},
                            {"name": "fusion::seg1", "source": TRIVIAL_C},
                            {"name": "fusion::seg2", "source": TRIVIAL_C},
                        ]
                    }
                ),
            )

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = main(
                    [
                        "measure",
                        "--rtsl",
                        str(model),
                        "--tasks-json",
                        str(tasks),
                        "--response-file",
                        str(response),
                        "--execute",
                        "--ptpn",
                        PTPN_BIN,
                    ]
                )
            self.assertEqual(code, 0, stdout.getvalue())
            report = json.loads(stdout.getvalue())
            self.assertEqual(report["status"], "ok")
            self.assertEqual(report["recheck"]["exit_code"], 0)
            self.assertEqual(report["recheck"]["pending_measurements"], [])
            self.assertEqual(len(report["tasks"][0]["segments_ms"]), 3)
            for low, high in report["tasks"][0]["segments_ms"]:
                self.assertGreaterEqual(low, 1)
                self.assertGreaterEqual(high, low)

            text = model.read_text(encoding="utf-8")
            self.assertIn("runs 2, timeout 5", text)
            self.assertRegex(
                text,
                r"time \[ \[\d+, \d+\], \[\d+, \d+\], \[\d+, \d+\] \] measured by llm",
            )
            self.assertRegex(
                text,
                r'artifact "sha256:[0-9a-f]{64}(,[0-9a-f]{64}){2} cc:[^"]+"',
            )
            self.assertNotIn("time measured by llm", text)

            code, envelope, _ = self._check(model)
            self.assertEqual(code, 0)
            self.assertEqual(envelope["pending_measurements"], [])

            analyzed = subprocess.run(
                [PTPN_BIN, "rtsl", "-f", str(model)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(analyzed.returncode, 0, analyzed.stderr.decode("utf-8", "replace"))

    @unittest.skipUnless(CC_AVAILABLE, "no C compiler available")
    def test_execute_with_out_preserves_the_input(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.rtsl"
            tasks = Path(directory) / "tasks.json"
            response = Path(directory) / "response.json"
            out = Path(directory) / "measured.rtsl"
            _write(model, SINGLE_RTSL)
            _write(tasks, json.dumps({"post": "consume a sample"}))
            _write(
                response,
                json.dumps({"tasks": [{"name": "post", "source": TRIVIAL_C}]}),
            )
            before = model.read_bytes()

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = main(
                    [
                        "measure",
                        "--rtsl",
                        str(model),
                        "--tasks-json",
                        str(tasks),
                        "--response-file",
                        str(response),
                        "--execute",
                        "--out",
                        str(out),
                        "--ptpn",
                        PTPN_BIN,
                    ]
                )
            self.assertEqual(code, 0, stdout.getvalue())
            report = json.loads(stdout.getvalue())
            self.assertEqual(report["status"], "ok")
            self.assertEqual(report["out"], str(out))
            self.assertFalse(report["in_place"])
            self.assertEqual(model.read_bytes(), before)
            self.assertTrue(out.is_file())
            self.assertIn("measured by llm", out.read_text(encoding="utf-8"))
            self.assertNotIn("time measured by llm", out.read_text(encoding="utf-8"))

            code, envelope, _ = self._check(out)
            self.assertEqual(code, 0)
            self.assertEqual(envelope["pending_measurements"], [])

    @unittest.skipUnless(CC_AVAILABLE, "no C compiler available")
    def test_response_file_with_wrong_task_set_does_not_modify_the_model(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.rtsl"
            tasks = Path(directory) / "tasks.json"
            response = Path(directory) / "response.json"
            _write(model, LOCKED_RTSL)
            _write(tasks, json.dumps({"fusion": "process one sample"}))
            _write(
                response,
                json.dumps({"tasks": [{"name": "fusion", "source": TRIVIAL_C}]}),
            )
            before = model.read_bytes()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = main(
                    [
                        "measure",
                        "--rtsl",
                        str(model),
                        "--tasks-json",
                        str(tasks),
                        "--response-file",
                        str(response),
                        "--execute",
                        "--ptpn",
                        PTPN_BIN,
                    ]
                )
            self.assertEqual(code, 2)
            self.assertIn("fusion::seg0", stderr.getvalue())
            self.assertEqual(model.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
