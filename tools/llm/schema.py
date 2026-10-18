"""Strict validation of task specifications and LLM responses.

The validator rejects unknown fields instead of silently ignoring them.  This
is deliberate: the LLM output is untrusted, and a misspelled declaration must
not silently change the model that is measured and analysed.
"""

import re
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Set, Tuple

DEFAULT_GRAPH_NAME = "LLMGeneratedTaskGraph"
SUPPORTED_POLICIES = frozenset(
    (
        "fixed",
        "fixed_prior_with_resume",
        "fixed_prior_with_restart",
        "rm",
        "dm",
        "edf",
        "llf",
        "fifo",
        "pip",
        "pcp",
        "srp",
    )
)
LOCK_PREFIXES = ("mutex", "spin")
MAX_SOURCE_CHARS = 1_000_000

SPEC_KEYS = frozenset(
    (
        "name",
        "num_cpus",
        "cores_per_cpu",
        "shared_locks",
        "policy",
        "start",
        "end",
        "periodic",
        "tasks",
    )
)
SPEC_TASK_KEYS = frozenset(
    (
        "name",
        "description",
        "period",
        "deadline",
        "priority",
        "allowed_cores",
        "locks",
        "depends_on",
    )
)
MODEL_KEYS = frozenset(("tasks",))
MODEL_TASK_KEYS = frozenset(("name", "source", "depends_on", "locks", "allowed_cores"))
MODEL_OPTIONAL_TASK_KEYS = ("depends_on", "locks", "allowed_cores")

_MAIN_RE = re.compile(r"\b(?:int|void)\s+main\s*\(")


class SchemaError(ValueError):
    """Raised when a spec or model response violates the contract."""

    def __init__(self, errors: Any) -> None:
        if isinstance(errors, str):
            messages = [errors]
        else:
            messages = list(errors)
        self.errors = messages
        super().__init__("; ".join(messages))


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _add(errors: List[str], path: str, message: str) -> None:
    errors.append("{}: {}".format(path, message))


def _check_unknown_keys(
    obj: Mapping[str, Any], allowed: Iterable[str], path: str, errors: List[str]
) -> None:
    allowed_set = set(allowed)
    for key in obj:
        if key not in allowed_set:
            _add(errors, path, "unknown field {!r}".format(key))


def _required_string(obj: Mapping[str, Any], key: str, path: str, errors: List[str]) -> Optional[str]:
    if key not in obj:
        _add(errors, path, "missing required field {!r}".format(key))
        return None
    value = obj[key]
    if not _is_nonempty_string(value):
        _add(errors, "{}.{}".format(path, key), "expected a non-empty string")
        return None
    return value


def _required_int(
    obj: Mapping[str, Any],
    key: str,
    path: str,
    errors: List[str],
    minimum: Optional[int] = None,
) -> Optional[int]:
    if key not in obj:
        _add(errors, path, "missing required field {!r}".format(key))
        return None
    value = obj[key]
    if not _is_int(value):
        _add(errors, "{}.{}".format(path, key), "expected an integer")
        return None
    if minimum is not None and value < minimum:
        _add(
            errors,
            "{}.{}".format(path, key),
            "must be >= {}, got {}".format(minimum, value),
        )
        return None
    return value


def _optional_int(
    obj: Mapping[str, Any],
    key: str,
    default: int,
    path: str,
    errors: List[str],
    minimum: Optional[int] = None,
) -> Optional[int]:
    if key not in obj:
        return default
    value = obj[key]
    if not _is_int(value):
        _add(errors, "{}.{}".format(path, key), "expected an integer")
        return None
    if minimum is not None and value < minimum:
        _add(
            errors,
            "{}.{}".format(path, key),
            "must be >= {}, got {}".format(minimum, value),
        )
        return None
    return value


def _int_list(
    value: Any,
    path: str,
    errors: List[str],
    allow_empty: bool,
    minimum: Optional[int] = None,
) -> Optional[List[int]]:
    if not isinstance(value, list):
        _add(errors, path, "expected an array of integers")
        return None
    if not value and not allow_empty:
        _add(errors, path, "must not be empty")
        return None
    result = []  # type: List[int]
    seen = set()  # type: Set[int]
    for index, item in enumerate(value):
        item_path = "{}[{}]".format(path, index)
        if not _is_int(item):
            _add(errors, item_path, "expected an integer")
            continue
        if minimum is not None and item < minimum:
            _add(errors, item_path, "must be >= {}, got {}".format(minimum, item))
            continue
        if item in seen:
            _add(errors, item_path, "duplicate value {}".format(item))
            continue
        seen.add(item)
        result.append(item)
    return result


def _string_list(
    value: Any,
    path: str,
    errors: List[str],
    allow_empty: bool,
    distinct: bool = True,
) -> Optional[List[str]]:
    if not isinstance(value, list):
        _add(errors, path, "expected an array of strings")
        return None
    if not value and not allow_empty:
        _add(errors, path, "must not be empty")
        return None
    result = []  # type: List[str]
    seen = set()  # type: Set[str]
    for index, item in enumerate(value):
        item_path = "{}[{}]".format(path, index)
        if not _is_nonempty_string(item):
            _add(errors, item_path, "expected a non-empty string")
            continue
        if distinct and item in seen:
            _add(errors, item_path, "duplicate value {!r}".format(item))
            continue
        seen.add(item)
        result.append(item)
    return result


def _lock_list(value: Any, path: str, errors: List[str]) -> Optional[List[str]]:
    locks = _string_list(value, path, errors, allow_empty=True)
    if locks is None:
        return None
    for index, lock in enumerate(locks):
        if not lock.startswith(LOCK_PREFIXES):
            _add(
                errors,
                "{}[{}]".format(path, index),
                "lock {!r} must start with 'mutex' or 'spin'".format(lock),
            )
    return locks


def _nonempty_text_list(value: Any, path: str, errors: List[str]) -> Optional[List[str]]:
    return _string_list(value, path, errors, allow_empty=True)


def _validate_start(value: Any, path: str, errors: List[str]) -> Optional[List[Dict[str, Any]]]:
    if not isinstance(value, list):
        _add(errors, path, "expected an array")
        return None
    result = []  # type: List[Dict[str, Any]]
    seen = set()  # type: Set[str]
    for index, item in enumerate(value):
        item_path = "{}[{}]".format(path, index)
        if isinstance(item, str):
            if not _is_nonempty_string(item):
                _add(errors, item_path, "expected a non-empty task name")
                continue
            task = item
            tokens = 1  # type: Optional[int]
        elif isinstance(item, dict):
            _check_unknown_keys(item, ("task", "tokens"), item_path, errors)
            task = _required_string(item, "task", item_path, errors)
            tokens = _optional_int(item, "tokens", 1, item_path, errors, minimum=0)
        else:
            _add(errors, item_path, "expected a task name or an object")
            continue
        if task is None:
            continue
        if task in seen:
            _add(errors, item_path, "duplicate start task {!r}".format(task))
            continue
        seen.add(task)
        result.append({"task": task, "tokens": 1 if tokens is None else tokens})
    return result


def _validate_end(value: Any, path: str, errors: List[str]) -> Optional[List[str]]:
    return _nonempty_text_list(value, path, errors)


def _validate_periodic(
    value: Any, path: str, errors: List[str]
) -> Optional[List[Dict[str, Any]]]:
    if not isinstance(value, list):
        _add(errors, path, "expected an array")
        return None
    result = []  # type: List[Dict[str, Any]]
    seen = set()  # type: Set[str]
    for index, item in enumerate(value):
        item_path = "{}[{}]".format(path, index)
        if not isinstance(item, dict):
            _add(errors, item_path, "expected an object")
            continue
        _check_unknown_keys(item, ("task", "period"), item_path, errors)
        task = _required_string(item, "task", item_path, errors)
        period = _required_int(item, "period", item_path, errors, minimum=1)
        if task is None:
            continue
        if task in seen:
            _add(errors, item_path, "duplicate periodic task {!r}".format(task))
            continue
        seen.add(task)
        result.append({"task": task, "period": 1 if period is None else period})
    return result


def _validate_task_spec(
    raw: Any, path: str, errors: List[str]
) -> Optional[Dict[str, Any]]:
    if not isinstance(raw, dict):
        _add(errors, path, "expected an object")
        return None
    _check_unknown_keys(raw, SPEC_TASK_KEYS, path, errors)

    name = _required_string(raw, "name", path, errors)
    description = _required_string(raw, "description", path, errors)
    period = _required_int(raw, "period", path, errors, minimum=0)
    deadline = _required_int(raw, "deadline", path, errors, minimum=0)
    priority = _required_int(raw, "priority", path, errors)

    if "allowed_cores" not in raw:
        _add(errors, path, "missing required field 'allowed_cores'")
        allowed_cores = None
    else:
        allowed_cores = _int_list(
            raw["allowed_cores"],
            "{}.allowed_cores".format(path),
            errors,
            allow_empty=False,
            minimum=0,
        )

    locks = []  # type: List[str]
    if "locks" in raw:
        maybe_locks = _lock_list(raw["locks"], "{}.locks".format(path), errors)
        if maybe_locks is not None:
            locks = maybe_locks

    depends_on = []  # type: List[str]
    if "depends_on" in raw:
        maybe_deps = _nonempty_text_list(
            raw["depends_on"], "{}.depends_on".format(path), errors
        )
        if maybe_deps is not None:
            depends_on = maybe_deps

    if name is not None and name in depends_on:
        _add(errors, "{}.depends_on".format(path), "task must not depend on itself")

    return {
        "name": name or "",
        "description": description or "",
        "period": 0 if period is None else period,
        "deadline": 0 if deadline is None else deadline,
        "priority": 0 if priority is None else priority,
        "allowed_cores": [] if allowed_cores is None else allowed_cores,
        "locks": locks,
        "depends_on": depends_on,
    }


def find_cycle(dep_map: Mapping[str, Sequence[str]]) -> Optional[List[str]]:
    """Return one dependency cycle as a node path, or ``None`` if acyclic."""
    color = dict((name, 0) for name in dep_map)
    for root in dep_map:
        if color[root] != 0:
            continue
        color[root] = 1
        path = [root]
        stack = [(root, iter(dep_map[root]))]  # type: List[Tuple[str, Any]]
        while stack:
            node, iterator = stack[-1]
            try:
                successor = next(iterator)
            except StopIteration:
                color[node] = 2
                stack.pop()
                path.pop()
                continue
            if successor not in dep_map:
                # Unknown targets are reported separately by the caller; they
                # cannot participate in a cycle in this graph.
                continue
            if color[successor] == 1:
                start = path.index(successor)
                return path[start:] + [successor]
            if color[successor] == 0:
                color[successor] = 1
                path.append(successor)
                stack.append((successor, iter(dep_map[successor])))
    return None


def _check_dependencies(
    dep_map: Mapping[str, Sequence[str]],
    known_names: Set[str],
    path_prefix: str,
    errors: List[str],
) -> None:
    for name, deps in dep_map.items():
        for index, dep in enumerate(deps):
            if dep not in known_names:
                _add(
                    errors,
                    "{}.{}.depends_on[{}]".format(path_prefix, name, index),
                    "unknown task {!r}".format(dep),
                )
            if dep == name:
                _add(
                    errors,
                    "{}.{}.depends_on[{}]".format(path_prefix, name, index),
                    "task must not depend on itself",
                )
    cycle = find_cycle(dep_map)
    if cycle is not None:
        _add(errors, path_prefix, "dependency cycle: {}".format(" -> ".join(cycle)))


def validate_spec(raw: Any) -> Dict[str, Any]:
    """Validate and normalize a task specification.

    Returns a new dictionary; the input is never mutated.  Missing optional
    task fields are filled with empty lists so downstream code has one shape.
    Optional graph-level fields (``start``, ``end``, ``periodic``,
    ``shared_locks``) are only present in the result when supplied, which lets
    the emitter distinguish an explicit empty list from a default.
    """
    if not isinstance(raw, dict):
        raise SchemaError("spec: expected a JSON object")

    errors = []  # type: List[str]
    _check_unknown_keys(raw, SPEC_KEYS, "spec", errors)

    num_cpus = _required_int(raw, "num_cpus", "spec", errors, minimum=1)
    cores_per_cpu = _required_int(raw, "cores_per_cpu", "spec", errors, minimum=1)

    name = raw.get("name", DEFAULT_GRAPH_NAME)
    if not _is_nonempty_string(name):
        _add(errors, "spec.name", "expected a non-empty string")
        name = DEFAULT_GRAPH_NAME

    policy = raw.get("policy", "fixed")
    if policy not in SUPPORTED_POLICIES:
        _add(
            errors,
            "spec.policy",
            "unsupported policy {!r}; expected one of {}".format(
                policy, ", ".join(sorted(SUPPORTED_POLICIES))
            ),
        )
        policy = "fixed"

    shared_locks = None  # type: Optional[List[str]]
    if "shared_locks" in raw:
        shared_locks = _lock_list(raw["shared_locks"], "spec.shared_locks", errors)

    start = None  # type: Optional[List[Dict[str, Any]]]
    if "start" in raw:
        start = _validate_start(raw["start"], "spec.start", errors)

    end = None  # type: Optional[List[str]]
    if "end" in raw:
        end = _validate_end(raw["end"], "spec.end", errors)

    periodic = None  # type: Optional[List[Dict[str, Any]]]
    if "periodic" in raw:
        periodic = _validate_periodic(raw["periodic"], "spec.periodic", errors)

    tasks_raw = raw.get("tasks")
    tasks = []  # type: List[Dict[str, Any]]
    if not isinstance(tasks_raw, list):
        _add(errors, "spec.tasks", "missing or invalid required field 'tasks' (expected an array)")
    elif not tasks_raw:
        _add(errors, "spec.tasks", "must contain at least one task")
    else:
        for index, item in enumerate(tasks_raw):
            task = _validate_task_spec(item, "spec.tasks[{}]".format(index), errors)
            if task is not None:
                tasks.append(task)

    known_names = set()  # type: Set[str]
    for index, task in enumerate(tasks):
        task_name = task["name"]
        if not task_name:
            continue
        if task_name in known_names:
            _add(
                errors,
                "spec.tasks[{}].name".format(index),
                "duplicate task name {!r}".format(task_name),
            )
            continue
        known_names.add(task_name)

    max_core = None
    if num_cpus is not None and cores_per_cpu is not None:
        # Real core ids come from num_cpus; cores_per_cpu is a capacity
        # parameter and does not create additional ids.
        max_core = num_cpus - 1

    for index, task in enumerate(tasks):
        task_path = "spec.tasks[{}]".format(index)
        if max_core is not None:
            for core_index, core in enumerate(task["allowed_cores"]):
                if core > max_core:
                    _add(
                        errors,
                        "{}.allowed_cores[{}]".format(task_path, core_index),
                        "core {} is outside 0..{}".format(core, max_core),
                    )
        if task["depends_on"]:
            for dep_index, dep in enumerate(task["depends_on"]):
                if dep not in known_names:
                    _add(
                        errors,
                        "{}.depends_on[{}]".format(task_path, dep_index),
                        "unknown task {!r}".format(dep),
                    )
        for lock_index, lock in enumerate(task["locks"]):
            if not lock.startswith(LOCK_PREFIXES):
                # Duplicate of the earlier check, kept here because the task
                # normalization is also reachable from model validation paths.
                _add(
                    errors,
                    "{}.locks[{}]".format(task_path, lock_index),
                    "invalid lock prefix in {!r}".format(lock),
                )
            if shared_locks is not None and lock not in shared_locks:
                _add(
                    errors,
                    "{}.locks[{}]".format(task_path, lock_index),
                    "lock {!r} is not declared in spec.shared_locks".format(lock),
                )

    dep_map = dict((task["name"], list(task["depends_on"])) for task in tasks if task["name"])
    _check_dependencies(dep_map, known_names, "spec", errors)

    if start is not None:
        for index, binding in enumerate(start):
            if binding["task"] not in known_names:
                _add(
                    errors,
                    "spec.start[{}].task".format(index),
                    "unknown task {!r}".format(binding["task"]),
                )
    if end is not None:
        for index, task_name in enumerate(end):
            if task_name not in known_names:
                _add(
                    errors,
                    "spec.end[{}]".format(index),
                    "unknown task {!r}".format(task_name),
                )
    if periodic is not None:
        for index, binding in enumerate(periodic):
            if binding["task"] not in known_names:
                _add(
                    errors,
                    "spec.periodic[{}].task".format(index),
                    "unknown task {!r}".format(binding["task"]),
                )

    if errors:
        raise SchemaError(errors)

    normalized = {
        "name": name,
        "num_cpus": num_cpus,
        "cores_per_cpu": cores_per_cpu,
        "policy": policy,
        "tasks": tasks,
    }  # type: Dict[str, Any]
    if shared_locks is not None:
        normalized["shared_locks"] = sorted(shared_locks)
    if start is not None:
        normalized["start"] = start
    if end is not None:
        normalized["end"] = end
    if periodic is not None:
        normalized["periodic"] = periodic
    return normalized


def coerce_spec(spec: Any) -> Dict[str, Any]:
    """Return a validated spec, accepting either raw or normalized input."""
    if not isinstance(spec, dict):
        raise SchemaError("spec: expected a JSON object")
    return validate_spec(spec)


def _validate_model_task(
    raw: Any, path: str, errors: List[str]
) -> Optional[Dict[str, Any]]:
    if not isinstance(raw, dict):
        _add(errors, path, "expected an object")
        return None
    _check_unknown_keys(raw, MODEL_TASK_KEYS, path, errors)

    name = _required_string(raw, "name", path, errors)
    source = _required_string(raw, "source", path, errors)
    if source is not None:
        if "\x00" in source:
            _add(errors, "{}.source".format(path), "must not contain NUL bytes")
        if len(source) > MAX_SOURCE_CHARS:
            _add(
                errors,
                "{}.source".format(path),
                "exceeds the {} character limit".format(MAX_SOURCE_CHARS),
            )
        if not _MAIN_RE.search(source):
            _add(
                errors,
                "{}.source".format(path),
                "must be a self-contained C program defining main()",
            )

    depends_on = []  # type: List[str]
    has_depends_on = "depends_on" in raw
    if has_depends_on:
        maybe_deps = _nonempty_text_list(
            raw["depends_on"], "{}.depends_on".format(path), errors
        )
        if maybe_deps is not None:
            depends_on = maybe_deps

    locks = []  # type: List[str]
    has_locks = "locks" in raw
    if has_locks:
        maybe_locks = _lock_list(raw["locks"], "{}.locks".format(path), errors)
        if maybe_locks is not None:
            locks = maybe_locks

    allowed_cores = None  # type: Optional[List[int]]
    has_allowed_cores = "allowed_cores" in raw
    if has_allowed_cores:
        allowed_cores = _int_list(
            raw["allowed_cores"],
            "{}.allowed_cores".format(path),
            errors,
            allow_empty=False,
            minimum=0,
        )

    if name is not None and name in depends_on:
        _add(errors, "{}.depends_on".format(path), "task must not depend on itself")

    normalized = {"name": name or "", "source": source or ""}  # type: Dict[str, Any]
    if has_depends_on:
        normalized["depends_on"] = depends_on
    if has_locks:
        normalized["locks"] = locks
    if has_allowed_cores and allowed_cores is not None:
        normalized["allowed_cores"] = allowed_cores
    return normalized


def validate_model_response(
    raw: Any, spec: Optional[Any] = None
) -> Dict[str, Any]:
    """Validate an LLM response object.

    When *spec* is supplied, the response must contain exactly the spec's task
    names, generated dependencies must stay inside that set and remain acyclic
    after merging, and any generated ``allowed_cores`` must be a subset of the
    corresponding spec declaration.
    """
    if not isinstance(raw, dict):
        raise SchemaError("response: expected a JSON object")
    validated_spec = coerce_spec(spec) if spec is not None else None

    errors = []  # type: List[str]
    _check_unknown_keys(raw, MODEL_KEYS, "response", errors)

    tasks_raw = raw.get("tasks")
    tasks = []  # type: List[Dict[str, Any]]
    if not isinstance(tasks_raw, list):
        _add(errors, "response.tasks", "missing or invalid required field 'tasks' (expected an array)")
    elif not tasks_raw:
        _add(errors, "response.tasks", "must contain at least one task")
    else:
        for index, item in enumerate(tasks_raw):
            task = _validate_model_task(item, "response.tasks[{}]".format(index), errors)
            if task is not None:
                tasks.append(task)

    known_names = set()  # type: Set[str]
    name_to_task = {}  # type: Dict[str, Dict[str, Any]]
    for index, task in enumerate(tasks):
        task_name = task["name"]
        if not task_name:
            continue
        if task_name in known_names:
            _add(
                errors,
                "response.tasks[{}].name".format(index),
                "duplicate task name {!r}".format(task_name),
            )
            continue
        known_names.add(task_name)
        name_to_task[task_name] = task

    dep_map = {}  # type: Dict[str, List[str]]
    for task in tasks:
        if not task["name"]:
            continue
        dep_map[task["name"]] = list(task.get("depends_on", []))

    _check_dependencies(dep_map, known_names, "response", errors)

    if validated_spec is not None:
        spec_names = set(task["name"] for task in validated_spec["tasks"])
        missing = sorted(spec_names - known_names)
        extra = sorted(known_names - spec_names)
        for name in missing:
            _add(errors, "response.tasks", "missing task {!r} required by the spec".format(name))
        for name in extra:
            _add(errors, "response.tasks", "unexpected task {!r} not present in the spec".format(name))

        spec_by_name = dict((task["name"], task) for task in validated_spec["tasks"])
        index_by_name = dict(
            (task["name"], index) for index, task in enumerate(validated_spec["tasks"])
        )
        for name in sorted(known_names & spec_names):
            generated = name_to_task[name]
            if "allowed_cores" in generated:
                allowed = set(spec_by_name[name]["allowed_cores"])
                for core_index, core in enumerate(generated["allowed_cores"]):
                    if core not in allowed:
                        _add(
                            errors,
                            "response.tasks[{}].allowed_cores[{}]".format(
                                index_by_name[name], core_index
                            ),
                            "core {} is not allowed by the spec ({})".format(
                                core, sorted(allowed)
                            ),
                        )

        merged = {}  # type: Dict[str, List[str]]
        for spec_task in validated_spec["tasks"]:
            name = spec_task["name"]
            generated = name_to_task.get(name)
            if generated is not None and "depends_on" in generated:
                merged[name] = list(generated["depends_on"])
            else:
                merged[name] = list(spec_task["depends_on"])
        cycle = find_cycle(merged)
        if cycle is not None:
            _add(errors, "response.tasks", "merged dependency cycle: {}".format(" -> ".join(cycle)))

    if errors:
        raise SchemaError(errors)
    return {"tasks": tasks}
