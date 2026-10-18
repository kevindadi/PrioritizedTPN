"""Prompt construction, safe measurement, and TDG emission.

The model is an untrusted proposer.  This module never trusts generated C
sources: they are compiled and run only when execution is explicitly enabled,
and only inside a temporary working directory with wall-clock timeouts and
best-effort POSIX resource limits.  The measured value is wall-clock elapsed
time, not a certified worst-case execution time.
"""

import json
import math
import os
import shutil
import signal
import subprocess
import tempfile
import time
from collections.abc import Mapping as MappingABC
from typing import Any, Dict, List, Optional, Sequence, Set, Tuple

from . import schema
from .client import call_client, extract_json_object

MAX_CAPTURED_OUTPUT = 65536
DEFAULT_WCET_METHOD = "wall-clock repeated runs (prototype; not certified)"

try:  # POSIX-only best-effort limits.
    import resource as _resource
except ImportError:  # pragma: no cover - exercised on non-POSIX hosts
    _resource = None


class MeasurementError(RuntimeError):
    """Structured error raised by :func:`measure_task`.

    ``stage`` is ``"disabled"``, ``"configuration"``, ``"compile"``, or
    ``"run"``.  Compiler diagnostics are preserved in ``stderr`` so callers can
    surface them without parsing free-form messages.
    """

    def __init__(
        self,
        stage: str,
        message: str,
        compiler: Optional[str] = None,
        returncode: Optional[int] = None,
        stdout: str = "",
        stderr: str = "",
        timed_out: bool = False,
    ) -> None:
        self.stage = stage
        self.message = message
        self.compiler = compiler
        self.returncode = returncode
        self.stdout = _truncate(stdout)
        self.stderr = _truncate(stderr)
        self.timed_out = timed_out
        super().__init__("{}: {}".format(stage, message))

    def to_dict(self) -> Dict[str, Any]:
        return {
            "stage": self.stage,
            "message": self.message,
            "compiler": self.compiler,
            "returncode": self.returncode,
            "timed_out": self.timed_out,
            "stdout": self.stdout,
            "stderr": self.stderr,
        }


def _truncate(text: str) -> str:
    if len(text) > MAX_CAPTURED_OUTPUT:
        return text[:MAX_CAPTURED_OUTPUT] + "\n...[truncated]"
    return text


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


_PROMPT_HEADER = """You are a proposal engine for a real-time task-graph analysis tool.
You do not certify timing and you must not claim a worst-case execution time
(WCET).  The tool validates your output, compiles and runs each C program, and
measures wall-clock elapsed time.  Treat the measured values as observations,
not as an upper bound.

The task specification is:

```json
{spec_json}
```

Return exactly one JSON object with this shape:

{
  "tasks": [
    {
      "name": "<task name from the specification>",
      "source": "<complete C program as a JSON string>",
      "depends_on": ["<task name>"],
      "locks": ["mutex0"],
      "allowed_cores": [0]
    }
  ]
}

Output contract:

- `tasks` must contain exactly one object for every task in the specification,
  using exactly the same task names.  Do not add or omit tasks.
- `name` and `source` are required.  `depends_on`, `locks`, and
  `allowed_cores` are optional.
- `source` must be a self-contained C program:
  * include every header it uses and define `int main(void)` (or
    `int main(int argc, char **argv)`);
  * exercise a representative worst-case path of the task on every run;
  * be deterministic, terminate on its own, and exit with status 0;
  * read no files, use no network, and require no command-line input;
  * use only the C standard library and ordinary computation.
- `depends_on` may reference only task names present in the specification.
  Dependencies must remain acyclic.  If omitted, the specification's
  dependency hint is used.  If present, it replaces that hint.
- `locks` must contain names starting with `mutex` or `spin`.  If omitted, the
  specification's lock hint is used.  If present, it replaces that hint.
- `allowed_cores`, when present, must be a non-empty subset of the cores the
  specification allows for that task.  It may narrow but never widen the
  affinity.  If omitted, the specification's affinity is used.
- Do not return timing numbers, WCET estimates, explanations, or Markdown
  fences.  Return only the JSON object."""


def build_prompt(spec: Any) -> str:
    """Build a deterministic prompt for *spec*.

    The prompt embeds the normalized spec with sorted object keys, so two
    equivalent inputs produce the same text.
    """
    validated = schema.validate_spec(spec)
    spec_json = json.dumps(validated, indent=2, sort_keys=True)
    return _PROMPT_HEADER.replace("{spec_json}", spec_json)


def parse_model_text(spec: Any, text: str) -> Dict[str, Any]:
    """Validate a saved LLM response against *spec* (offline replay helper)."""
    validated = schema.validate_spec(spec)
    raw = extract_json_object(text)
    return schema.validate_model_response(raw, spec=validated)


def generate_model(spec: Any, client: Any) -> Dict[str, Any]:
    """Ask *client* for a model and return the validated response dictionary.

    *client* may be an object with ``complete(prompt)`` or any callable taking
    the prompt string.  Network access is entirely the client's concern.
    """
    validated = schema.validate_spec(spec)
    prompt = build_prompt(validated)
    text = call_client(client, prompt)
    return parse_model_text(validated, text)


def _sanitized_env() -> Dict[str, str]:
    """Environment for compiler and task processes, with secrets removed."""
    markers = ("API_KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL")
    env = {}  # type: Dict[str, str]
    for key, value in os.environ.items():
        upper = key.upper()
        if upper.startswith("PTPN_LLM_"):
            continue
        if any(marker in upper for marker in markers):
            continue
        env[key] = value
    return env


def _apply_resource_limits(timeout_s: float) -> None:
    """Best-effort CPU/address-space/file-size limits for a task process."""
    if _resource is None:
        return
    cpu_seconds = max(1, int(math.ceil(timeout_s)))
    limits = [(_resource.RLIMIT_CPU, (cpu_seconds, cpu_seconds + 1))]
    if hasattr(_resource, "RLIMIT_AS"):
        limits.append((_resource.RLIMIT_AS, (512 * 1024 * 1024, 512 * 1024 * 1024)))
    if hasattr(_resource, "RLIMIT_FSIZE"):
        limits.append((_resource.RLIMIT_FSIZE, (1024 * 1024, 1024 * 1024)))
    if hasattr(_resource, "RLIMIT_CORE"):
        limits.append((_resource.RLIMIT_CORE, (0, 0)))
    for resource_name, resource_value in limits:
        try:
            _resource.setrlimit(resource_name, resource_value)
        except (ValueError, OSError):
            # Hard limits cannot always be raised in constrained environments;
            # the wall-clock timeout remains in force.
            pass


def _kill_process_group(process: subprocess.Popen) -> None:
    try:
        if os.name == "posix":
            group_id = os.getpgid(process.pid)
            # The child calls setsid() before exec, so group_id == pid.
            # Never signal a group that is not private to the child: doing so
            # could kill the Python process itself.
            if group_id == process.pid:
                os.killpg(group_id, signal.SIGKILL)
            else:
                process.kill()
        else:  # pragma: no cover - non-POSIX fallback
            process.kill()
    except OSError:
        try:
            process.kill()
        except OSError:
            pass


def _read_capped(handle: Any) -> str:
    handle.seek(0)
    data = handle.read(MAX_CAPTURED_OUTPUT + 1)
    text = data.decode("utf-8", "replace")
    if len(data) > MAX_CAPTURED_OUTPUT:
        text += "\n...[truncated]"
    return text


def _run_command(
    command: Sequence[str],
    cwd: str,
    timeout_s: float,
    stage: str,
    apply_limits: bool,
) -> Tuple[int, str, str]:
    """Run *command* without a shell, enforcing a wall-clock timeout.

    Child output is redirected to temporary files rather than pipes so that a
    program that writes without bound cannot grow the parent process's memory.
    """
    env = _sanitized_env()
    kwargs = {
        "cwd": cwd,
        "stdin": subprocess.DEVNULL,
        "env": env,
    }  # type: Dict[str, Any]
    if os.name == "posix":
        def preexec() -> None:
            try:
                os.setsid()
            except OSError:
                pass
            if apply_limits:
                _apply_resource_limits(timeout_s)

        kwargs["preexec_fn"] = preexec

    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        kwargs["stdout"] = stdout_file
        kwargs["stderr"] = stderr_file
        try:
            process = subprocess.Popen(list(command), **kwargs)
        except OSError as exc:
            raise MeasurementError(
                stage,
                "could not start {!r}: {}".format(command[0], exc),
            )

        timed_out = False
        try:
            process.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            timed_out = True
            _kill_process_group(process)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:  # pragma: no cover - defensive
                pass

        stdout = _read_capped(stdout_file)
        stderr = _read_capped(stderr_file)
        if timed_out:
            raise MeasurementError(
                stage,
                "timed out after {:.3f} seconds".format(timeout_s),
                returncode=process.returncode,
                stdout=stdout,
                stderr=stderr,
                timed_out=True,
            )
        return int(process.returncode), stdout, stderr


def measure_task(
    source: str,
    runs: int = 20,
    timeout_s: float = 5,
    compiler: str = "cc",
    opt: str = "-O2",
    allow_execute: bool = False,
) -> Dict[str, int]:
    """Compile and repeatedly run *source*, returning ``{"lo", "hi"}`` in us.

    The source is written to a fresh temporary directory, compiled with an
    argument list (never a shell), and executed ``runs`` times.  Each execution
    is bounded by ``timeout_s`` seconds of wall-clock time and, on POSIX, by
    best-effort ``resource`` limits.  Execution is refused unless
    ``allow_execute`` is true.

    The returned interval is ``[min, max]`` of observed wall-clock elapsed
    times in integer microseconds.  It includes process creation overhead and
    is **not** a WCET bound.
    """
    if not allow_execute:
        raise MeasurementError(
            "disabled",
            "compiling and executing model-generated code requires "
            "allow_execute=True (CLI: --execute)",
        )
    if not isinstance(source, str) or not source.strip():
        raise MeasurementError("configuration", "source must be a non-empty string")
    if not _is_int(runs) or runs < 1:
        raise MeasurementError("configuration", "runs must be a positive integer")
    if not isinstance(compiler, str) or not compiler.strip():
        raise MeasurementError("configuration", "compiler must be a non-empty string")
    if not isinstance(opt, str):
        raise MeasurementError("configuration", "opt must be a string")
    try:
        timeout = float(timeout_s)
    except (TypeError, ValueError):
        raise MeasurementError("configuration", "timeout_s must be a positive number")
    if timeout <= 0:
        raise MeasurementError("configuration", "timeout_s must be positive")
    if not math.isfinite(timeout):
        raise MeasurementError("configuration", "timeout_s must be finite")

    workdir = tempfile.mkdtemp(prefix="ptpn_llm_measure_")
    try:
        source_path = os.path.join(workdir, "task.c")
        executable_path = os.path.join(workdir, "task_bin")
        with open(source_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(source)

        compile_command = [compiler]
        if opt:
            compile_command.append(opt)
        compile_command.extend(
            ["-std=c11", "-Wall", "-Wextra", "-o", executable_path, source_path, "-lm"]
        )
        compile_timeout = max(30.0, timeout * 4.0)
        returncode, stdout, stderr = _run_command(
            compile_command,
            cwd=workdir,
            timeout_s=compile_timeout,
            stage="compile",
            apply_limits=False,
        )
        if returncode != 0:
            raise MeasurementError(
                "compile",
                "compiler exited with status {}".format(returncode),
                compiler=compiler,
                returncode=returncode,
                stdout=stdout,
                stderr=stderr,
            )
        if not os.path.isfile(executable_path):
            raise MeasurementError(
                "compile",
                "compiler did not produce an executable",
                compiler=compiler,
                returncode=returncode,
                stdout=stdout,
                stderr=stderr,
            )

        samples = []  # type: List[int]
        for _ in range(runs):
            started_ns = time.perf_counter_ns()
            returncode, stdout, stderr = _run_command(
                [executable_path],
                cwd=workdir,
                timeout_s=timeout,
                stage="run",
                apply_limits=True,
            )
            elapsed_ns = time.perf_counter_ns() - started_ns
            if returncode != 0:
                raise MeasurementError(
                    "run",
                    "program exited with status {}".format(returncode),
                    compiler=compiler,
                    returncode=returncode,
                    stdout=stdout,
                    stderr=stderr,
                )
            samples.append(elapsed_ns)

        return {"lo": min(samples) // 1000, "hi": max(samples) // 1000}
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def _coerce_interval(task_name: str, value: Any, errors: List[str]) -> Optional[Tuple[int, int]]:
    if isinstance(value, MappingABC):
        lo = value.get("lo")
        hi = value.get("hi")
        extra = set(value) - set(("lo", "hi"))
        if extra:
            errors.append(
                "measured[{!r}]: unknown field(s) {}".format(
                    task_name, ", ".join(sorted(repr(key) for key in extra))
                )
            )
    elif isinstance(value, (list, tuple)) and len(value) == 2 and not isinstance(value, str):
        lo, hi = value
    else:
        errors.append(
            "measured[{!r}]: expected {{'lo': int, 'hi': int}} or [lo, hi]".format(
                task_name
            )
        )
        return None
    if not _is_int(lo) or not _is_int(hi):
        errors.append("measured[{!r}]: lo and hi must be integers".format(task_name))
        return None
    if lo < 0 or hi < 0:
        errors.append("measured[{!r}]: lo and hi must be non-negative".format(task_name))
        return None
    if hi < lo:
        errors.append("measured[{!r}]: hi must be >= lo".format(task_name))
        return None
    return int(lo), int(hi)


def _is_interval_pair(value: Any) -> bool:
    """True when *value* is a single ``[lo, hi]`` pair or ``{'lo','hi'}``."""
    if isinstance(value, MappingABC):
        return True
    return (
        isinstance(value, (list, tuple))
        and len(value) == 2
        and all(_is_int(item) for item in value)
    )


def _coerce_segments(
    task_name: str, value: Any, expected: int, errors: List[str]
) -> Optional[List[Tuple[int, int]]]:
    """Coerce a measurement into exactly *expected* ``(lo, hi)`` segments.

    Lock-free tasks take one total interval. Tasks that declare locks must
    supply one interval per TDG segment (``2 * locks + 1``); silently putting a
    total interval in the first segment would model the critical sections as
    zero-time and remove lock contention from the analysis.
    """
    if expected == 1:
        interval = _coerce_interval(task_name, value, errors)
        return None if interval is None else [interval]

    if _is_interval_pair(value):
        errors.append(
            "measured[{!r}]: task declares locks; provide {} segment intervals "
            "([lo, hi] per TDG segment), not one total interval".format(
                task_name, expected
            )
        )
        return None
    if not isinstance(value, (list, tuple)):
        errors.append(
            "measured[{!r}]: expected a list of {} segment intervals".format(
                task_name, expected
            )
        )
        return None
    if len(value) != expected:
        errors.append(
            "measured[{!r}]: expected {} segment intervals, got {}".format(
                task_name, expected, len(value)
            )
        )
        return None

    segments = []  # type: List[Tuple[int, int]]
    for index, item in enumerate(value):
        interval = _coerce_interval("{}[{}]".format(task_name, index), item, errors)
        if interval is None:
            return None
        segments.append(interval)
    return segments


def _merged_declarations(
    spec: Dict[str, Any], generated: Dict[str, Any]
) -> Tuple[Dict[str, List[str]], Dict[str, List[str]], Dict[str, List[int]]]:
    generated_by_name = dict((task["name"], task) for task in generated["tasks"])
    dependencies = {}  # type: Dict[str, List[str]]
    locks = {}  # type: Dict[str, List[str]]
    cores = {}  # type: Dict[str, List[int]]
    for task in spec["tasks"]:
        name = task["name"]
        proposal = generated_by_name[name]
        dependencies[name] = list(
            proposal["depends_on"] if "depends_on" in proposal else task["depends_on"]
        )
        locks[name] = list(proposal["locks"] if "locks" in proposal else task["locks"])
        cores[name] = list(
            proposal["allowed_cores"]
            if "allowed_cores" in proposal
            else task["allowed_cores"]
        )
    return dependencies, locks, cores


def emit_tdg(spec: Any, generated: Any, measured: Any) -> Dict[str, Any]:
    """Emit a TDG JSON object from a spec, validated model, and measurements.

    ``measured`` maps task name to ``{"lo": int, "hi": int}`` (microseconds),
    a two-element ``[lo, hi]`` list, or (for tasks that declare locks) a list of
    ``2 * len(locks) + 1`` segment intervals.  Every task must have a
    measurement.  The result matches the field names used by
    ``docs/json_format.md``:

    * ``time`` contains ``2 * len(locks) + 1`` intervals, as required by the
      existing parser. Lock-free tasks use their total interval. Tasks with
      locks must supply per-segment measurements: the emitter refuses a single
      total interval, because putting it in the first segment would model every
      critical section as zero-time.
    * task nodes carry ``wcet_kind: "measured"`` and an explicit
      ``wcet_method`` string.
    * dependency edges use the ``{"source", "target", "label"}`` shape from
      the repository examples.

    This function does not run anything; measurement is a separate step.
    """
    validated_spec = schema.validate_spec(spec)
    validated_model = schema.validate_model_response(generated, spec=validated_spec)
    dependencies, locks, cores = _merged_declarations(validated_spec, validated_model)

    task_names = [task["name"] for task in validated_spec["tasks"]]
    known = set(task_names)
    if not isinstance(measured, MappingABC):
        raise schema.SchemaError("measured: expected an object mapping task names to intervals")
    missing = [name for name in task_names if name not in measured]
    extra = [key for key in measured if key not in known]
    errors = []  # type: List[str]
    if missing:
        errors.append("measured: missing interval(s) for {}".format(", ".join(missing)))
    if extra:
        errors.append(
            "measured: unexpected task(s) {}".format(
                ", ".join(repr(key) for key in extra)
            )
        )
    intervals = {}  # type: Dict[str, List[Tuple[int, int]]]
    for name in task_names:
        if name in measured:
            segments = _coerce_segments(
                name, measured[name], 2 * len(locks[name]) + 1, errors
            )
            if segments is not None:
                intervals[name] = segments
    if errors:
        raise schema.SchemaError(errors)

    # A cycle can only arise from generated overrides; validate_model_response
    # already checked the merged graph, but keep the emitter self-contained.
    cycle = schema.find_cycle(dependencies)
    if cycle is not None:
        raise schema.SchemaError(
            "merged dependency cycle: {}".format(" -> ".join(cycle))
        )

    if "start" in validated_spec:
        start = [
            {"task": binding["task"], "tokens": binding["tokens"]}
            for binding in validated_spec["start"]
        ]
    else:
        start = [
            {"task": task["name"], "tokens": 1}
            for task in validated_spec["tasks"]
            if not dependencies[task["name"]] and task["period"] == 0
        ]

    if "end" in validated_spec:
        end = list(validated_spec["end"])
    else:
        has_outgoing = set()  # type: Set[str]
        for task_dependencies in dependencies.values():
            has_outgoing.update(task_dependencies)
        end = [name for name in task_names if name not in has_outgoing]

    if "periodic" in validated_spec:
        periodic = [
            {"task": binding["task"], "period": binding["period"]}
            for binding in validated_spec["periodic"]
        ]
    else:
        periodic = [
            {"task": task["name"], "period": task["period"]}
            for task in validated_spec["tasks"]
            if task["period"] > 0
        ]

    shared_locks = set(validated_spec.get("shared_locks", []))
    for task_locks in locks.values():
        shared_locks.update(task_locks)

    nodes = []  # type: List[Dict[str, Any]]
    for task in validated_spec["tasks"]:
        name = task["name"]
        segments = intervals[name]
        task_locks = locks[name]
        allowed_cores = cores[name]
        nodes.append(
            {
                "type": "task",
                "id": name,
                "priority": task["priority"],
                "deadline": task["deadline"],
                "core": allowed_cores[0],
                "time": [[lo, hi] for lo, hi in segments],
                "allowed_cores": list(allowed_cores),
                "locks": list(task_locks),
                "wcet_kind": "measured",
                "wcet_method": DEFAULT_WCET_METHOD,
            }
        )

    edges = []  # type: List[Dict[str, str]]
    for task in validated_spec["tasks"]:
        target = task["name"]
        for source in dependencies[target]:
            edges.append({"source": source, "target": target, "label": "0,0"})

    return {
        "graph": {"name": validated_spec["name"]},
        "configuration": {
            "num_cpus": validated_spec["num_cpus"],
            "cores_per_cpu": validated_spec["cores_per_cpu"],
            "shared_locks": sorted(shared_locks),
            "policy": validated_spec["policy"],
            "start": start,
            "end": end,
            "periodic": periodic,
        },
        "nodes": nodes,
        "edges": edges,
    }
