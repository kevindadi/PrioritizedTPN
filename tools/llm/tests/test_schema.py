"""Tests for strict spec and model-response validation."""

import copy
import sys
import unittest
from pathlib import Path

_REPO_ROOT = str(Path(__file__).resolve().parents[3])
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from tools.llm.schema import (  # noqa: E402
    SchemaError,
    find_cycle,
    validate_model_response,
    validate_spec,
)


def base_spec():
    return {
        "name": "Demo",
        "num_cpus": 2,
        "cores_per_cpu": 1,
        "shared_locks": ["mutex0"],
        "policy": "fixed",
        "start": [{"task": "A", "tokens": 1}],
        "end": ["B"],
        "periodic": [{"task": "A", "period": 100}],
        "tasks": [
            {
                "name": "A",
                "description": "root task",
                "period": 100,
                "deadline": 100,
                "priority": 5,
                "allowed_cores": [0],
            },
            {
                "name": "B",
                "description": "leaf task",
                "period": 0,
                "deadline": 50,
                "priority": 4,
                "allowed_cores": [0, 1],
                "locks": ["mutex0"],
                "depends_on": ["A"],
            },
        ],
    }


def model_response():
    return {
        "tasks": [
            {
                "name": "A",
                "source": "int main(void) { return 0; }",
            },
            {
                "name": "B",
                "source": "int main(void) { return 0; }",
                "depends_on": ["A"],
                "locks": ["mutex0"],
                "allowed_cores": [1],
            },
        ]
    }


class SpecValidationTests(unittest.TestCase):
    def test_valid_spec_is_normalized(self):
        raw = base_spec()
        original = copy.deepcopy(raw)
        normalized = validate_spec(raw)
        self.assertEqual(raw, original)
        self.assertEqual(normalized["name"], "Demo")
        self.assertEqual(normalized["policy"], "fixed")
        self.assertEqual(normalized["tasks"][1]["locks"], ["mutex0"])
        self.assertEqual(normalized["tasks"][0]["depends_on"], [])

    def test_idempotent_normalization(self):
        once = validate_spec(base_spec())
        twice = validate_spec(once)
        self.assertEqual(once, twice)

    def test_unknown_top_level_field(self):
        raw = base_spec()
        raw["unexpected"] = 1
        with self.assertRaises(SchemaError) as raised:
            validate_spec(raw)
        self.assertIn("unexpected", str(raised.exception))

    def test_unknown_task_field(self):
        raw = base_spec()
        raw["tasks"][0]["wcet"] = 10
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_missing_required_task_field(self):
        raw = base_spec()
        del raw["tasks"][0]["description"]
        with self.assertRaises(SchemaError) as raised:
            validate_spec(raw)
        self.assertIn("description", str(raised.exception))

    def test_boolean_is_not_an_integer(self):
        raw = base_spec()
        raw["tasks"][0]["period"] = True
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_negative_period(self):
        raw = base_spec()
        raw["tasks"][0]["period"] = -1
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_allowed_cores_must_be_nonempty_distinct_nonnegative(self):
        for value in ([], [0, 0], [-1]):
            raw = base_spec()
            raw["tasks"][0]["allowed_cores"] = value
            with self.assertRaises(SchemaError, msg=repr(value)):
                validate_spec(raw)

    def test_allowed_cores_out_of_range(self):
        raw = base_spec()
        raw["tasks"][0]["allowed_cores"] = [2]
        with self.assertRaises(SchemaError) as raised:
            validate_spec(raw)
        self.assertIn("outside 0..1", str(raised.exception))

    def test_lock_prefix_is_enforced(self):
        raw = base_spec()
        raw["tasks"][1]["locks"] = ["semaphore0"]
        raw["shared_locks"] = ["semaphore0"]
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_lock_must_be_declared_when_shared_locks_are_given(self):
        raw = base_spec()
        raw["shared_locks"] = []
        with self.assertRaises(SchemaError) as raised:
            validate_spec(raw)
        self.assertIn("not declared", str(raised.exception))

    def test_unknown_dependency(self):
        raw = base_spec()
        raw["tasks"][1]["depends_on"] = ["missing"]
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_self_dependency(self):
        raw = base_spec()
        raw["tasks"][0]["depends_on"] = ["A"]
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_dependency_cycle(self):
        raw = base_spec()
        raw["tasks"][0]["depends_on"] = ["B"]
        with self.assertRaises(SchemaError) as raised:
            validate_spec(raw)
        self.assertIn("cycle", str(raised.exception))

    def test_start_binding_validation(self):
        raw = base_spec()
        raw["start"] = [{"task": "A", "tokens": -1}]
        with self.assertRaises(SchemaError):
            validate_spec(raw)

        raw = base_spec()
        raw["start"] = ["missing"]
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_periodic_validation(self):
        raw = base_spec()
        raw["periodic"] = [{"task": "A", "period": 0}]
        with self.assertRaises(SchemaError):
            validate_spec(raw)

        raw = base_spec()
        raw["periodic"] = [{"task": "missing", "period": 10}]
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_end_validation(self):
        raw = base_spec()
        raw["end"] = ["missing"]
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_policy_validation(self):
        raw = base_spec()
        raw["policy"] = "unknown-policy"
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_duplicate_task_names(self):
        raw = base_spec()
        raw["tasks"][1]["name"] = "A"
        with self.assertRaises(SchemaError):
            validate_spec(raw)

    def test_missing_graph_fields(self):
        raw = base_spec()
        del raw["num_cpus"]
        with self.assertRaises(SchemaError):
            validate_spec(raw)


class ModelResponseTests(unittest.TestCase):
    def test_valid_model(self):
        normalized = validate_model_response(model_response(), spec=base_spec())
        self.assertEqual([task["name"] for task in normalized["tasks"]], ["A", "B"])
        self.assertEqual(normalized["tasks"][1]["allowed_cores"], [1])

    def test_optional_declarations_are_preserved_only_when_present(self):
        raw = model_response()
        for key in ("depends_on", "locks", "allowed_cores"):
            del raw["tasks"][1][key]
        normalized = validate_model_response(raw, spec=base_spec())
        self.assertNotIn("depends_on", normalized["tasks"][1])
        self.assertNotIn("locks", normalized["tasks"][1])
        self.assertNotIn("allowed_cores", normalized["tasks"][1])

    def test_missing_source(self):
        raw = model_response()
        del raw["tasks"][0]["source"]
        with self.assertRaises(SchemaError):
            validate_model_response(raw, spec=base_spec())

    def test_source_must_define_main(self):
        raw = model_response()
        raw["tasks"][0]["source"] = "int helper(void) { return 0; }"
        with self.assertRaises(SchemaError) as raised:
            validate_model_response(raw, spec=base_spec())
        self.assertIn("main", str(raised.exception))

    def test_source_rejects_nul(self):
        raw = model_response()
        raw["tasks"][0]["source"] = "int main(void) { return 0; }\x00"
        with self.assertRaises(SchemaError):
            validate_model_response(raw, spec=base_spec())

    def test_unknown_top_level_field(self):
        raw = model_response()
        raw["notes"] = "ignored?"
        with self.assertRaises(SchemaError):
            validate_model_response(raw, spec=base_spec())

    def test_unknown_task_field(self):
        raw = model_response()
        raw["tasks"][0]["wcet"] = 100
        with self.assertRaises(SchemaError):
            validate_model_response(raw, spec=base_spec())

    def test_duplicate_model_task_names(self):
        raw = model_response()
        raw["tasks"][1]["name"] = "A"
        with self.assertRaises(SchemaError):
            validate_model_response(raw, spec=base_spec())

    def test_missing_and_extra_tasks(self):
        raw = model_response()
        raw["tasks"] = raw["tasks"][:1]
        with self.assertRaises(SchemaError) as raised:
            validate_model_response(raw, spec=base_spec())
        self.assertIn("missing task", str(raised.exception))

        raw = model_response()
        raw["tasks"].append(
            {"name": "C", "source": "int main(void) { return 0; }"}
        )
        with self.assertRaises(SchemaError) as raised:
            validate_model_response(raw, spec=base_spec())
        self.assertIn("unexpected task", str(raised.exception))

    def test_allowed_cores_must_narrow_spec_affinity(self):
        raw = model_response()
        raw["tasks"][1]["allowed_cores"] = [2]
        with self.assertRaises(SchemaError):
            validate_model_response(raw, spec=base_spec())

    def test_unknown_generated_dependency(self):
        raw = model_response()
        raw["tasks"][1]["depends_on"] = ["missing"]
        with self.assertRaises(SchemaError):
            validate_model_response(raw, spec=base_spec())

    def test_generated_dependency_cycle_after_merge(self):
        raw = model_response()
        raw["tasks"][0]["depends_on"] = ["B"]
        raw["tasks"][1]["depends_on"] = ["A"]
        with self.assertRaises(SchemaError) as raised:
            validate_model_response(raw, spec=base_spec())
        self.assertIn("cycle", str(raised.exception))

    def test_model_without_spec(self):
        normalized = validate_model_response(model_response())
        self.assertEqual(len(normalized["tasks"]), 2)

    def test_invalid_lock_prefix_in_model(self):
        raw = model_response()
        raw["tasks"][1]["locks"] = ["sem0"]
        with self.assertRaises(SchemaError):
            validate_model_response(raw, spec=base_spec())


class CycleHelperTests(unittest.TestCase):
    def test_cycle_detection(self):
        self.assertEqual(find_cycle({"A": ["B"], "B": ["A"]}), ["A", "B", "A"])
        self.assertIsNone(find_cycle({"A": ["B"], "B": []}))

    def test_unknown_targets_do_not_crash_cycle_detection(self):
        self.assertIsNone(find_cycle({"A": ["missing"], "B": []}))


if __name__ == "__main__":
    unittest.main()
