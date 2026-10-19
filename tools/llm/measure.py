"""RTSL measurement loop for ``measured by llm`` tasks.

The loop closes the gap between ``ptpn check`` and a fully timed model:

1. Run ``ptpn check -f model.rtsl --diagnostics json`` and read the versioned
   diagnostics envelope.  A failed check leaves the file untouched.
2. Read ``result.pending_measurements`` from a successful envelope.  Every
   entry carries the task name, ``expected_segments`` (``2 * locks + 1``),
   ``runs``, ``timeout``, and the byte span of the whole ``time`` attribute
   (``task:<id>:time`` in the SourceMap).
3. Ask an LLM (or replay a saved response) for one self-contained C program
   per measured segment.  The existing prototype prompt/validation/client
   machinery is reused unchanged: each segment is presented as a synthetic
   task in a one-CPU spec, so a multi-segment task yields several synthetic
   task names of the form ``<task>::seg<index>``.
4. Compile and run every segment with :func:`tools.llm.pipeline.measure_task`
   (temporary directory, wall-clock timeout, best-effort POSIX limits,
   scrubbed environment).  Execution happens only with ``--execute``.
5. Convert the observations to the RTSL millisecond base and replace the
   ``time`` attribute with a single byte-slice substitution, then re-run
   ``ptpn check`` and report that the model has no pending measurements.

The LLM output and the compiled programs remain untrusted: the measured
interval is a wall-clock observation, not a WCET upper bound.
"""

import json
import os
import re
import shlex
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

from .client import LLMError, client_from_env, strict_json_loads
from .pipeline import (
    DEFAULT_WCET_METHOD,
    MeasurementError,
    build_prompt,
    generate_model,
    measure_task,
    parse_model_text,
)
from .schema import SchemaError

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR_MARKER = ".ptpn-build-dir"
PTPN_EXE = "ptpn.exe" if os.name == "nt" else "ptpn"
PROTOCOL = "ptpn-diagnostics"
DEFAULT_RUNS = 20
DEFAULT_TIMEOUT_S = 5
DEFAULT_COMPILER = "cc"
DEFAULT_OPT = "-O2"
NS_PER_MS = 1_000_000

_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_SEGMENT_NAME_SEPARATOR = "::seg"


class MeasureError(RuntimeError):
    """Raised for pre-flight, envelope, injection, or re-check failures."""


class EnvelopeError(MeasureError):
    """Raised when ``ptpn check`` does not produce a usable envelope."""


class InjectionError(MeasureError):
    """Raised when an inject span is missing, stale, or unsafe to replace."""


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


# ---------------------------------------------------------------------------
# Binary resolution (mirrors example/run.py: --ptpn, PTPN_BUILD_DIR, marker, build/)
# ---------------------------------------------------------------------------


def resolve_ptpn_bin(
    explicit: Optional[str] = None,
    env: Optional[Mapping[str, str]] = None,
    repo_root: Optional[Path] = None,
) -> Path:
    """Resolve the ``ptpn`` executable using the repository's lookup order.

    Order: ``--ptpn`` -> ``$PTPN_BUILD_DIR`` -> ``<repo>/.ptpn-build-dir`` ->
    ``<repo>/build``.  Relative paths (explicit or environment) are resolved
    against the repository root, matching ``example/run.py``.
    """
    root = Path(repo_root) if repo_root is not None else REPO_ROOT
    source = os.environ if env is None else env

    if explicit:
        path = Path(explicit).expanduser()
        return (root / path).resolve() if not path.is_absolute() else path.resolve()

    build_dir = None  # type: Optional[Path]
    value = (source.get("PTPN_BUILD_DIR") or "").strip()
    if value:
        path = Path(value).expanduser()
        build_dir = (root / path).resolve() if not path.is_absolute() else path.resolve()
    else:
        marker = root / BUILD_DIR_MARKER
        if marker.is_file():
            try:
                line = marker.read_text(encoding="utf-8").strip()
            except OSError:
                line = ""
            if line:
                path = Path(line).expanduser()
                build_dir = (
                    (root / path).resolve() if not path.is_absolute() else path.resolve()
                )
    if build_dir is None:
        build_dir = (root / "build").resolve()
    return build_dir / PTPN_EXE


def ensure_ptpn_bin(path: Path) -> None:
    if path.is_file():
        return
    raise MeasureError(
        "ptpn binary not found: {}\n"
        "build first: example/build.py\n"
        "or set PTPN_BUILD_DIR / pass --ptpn PATH".format(path)
    )


# ---------------------------------------------------------------------------
# Diagnostics envelope parsing
# ---------------------------------------------------------------------------


def parse_envelope(text: str) -> Dict[str, Any]:
    """Parse and minimally validate a ``ptpn-diagnostics`` envelope.

    Unknown fields and unknown diagnostic codes are tolerated (protocol
    versioning policy).  Missing or malformed structural fields are errors.
    """
    try:
        raw = strict_json_loads(text)
    except (TypeError, ValueError) as exc:
        raise EnvelopeError("ptpn check did not emit valid JSON: {}".format(exc))
    if not isinstance(raw, dict):
        raise EnvelopeError("ptpn check envelope must be a JSON object")

    protocol = raw.get("protocol")
    if protocol != PROTOCOL:
        raise EnvelopeError(
            "unexpected diagnostics protocol {!r}; expected {!r}".format(protocol, PROTOCOL)
        )

    version = raw.get("version")
    if not _is_int(version) or version < 1:
        raise EnvelopeError("diagnostics envelope has an invalid 'version'")

    ok = raw.get("ok")
    if not isinstance(ok, bool):
        raise EnvelopeError("diagnostics envelope is missing a boolean 'ok'")

    exit_code = raw.get("exit_code")
    if not _is_int(exit_code):
        raise EnvelopeError("diagnostics envelope is missing an integer 'exit_code'")

    diagnostics = raw.get("diagnostics", [])
    if not isinstance(diagnostics, list):
        raise EnvelopeError("diagnostics envelope field 'diagnostics' must be an array")

    truncated = raw.get("truncated_diagnostics", False)
    if not isinstance(truncated, bool):
        raise EnvelopeError("field 'truncated_diagnostics' must be a boolean")

    result = raw.get("result", {})
    if not isinstance(result, dict):
        raise EnvelopeError("diagnostics envelope field 'result' must be an object")

    pending = result.get("pending_measurements", [])
    if not isinstance(pending, list):
        raise EnvelopeError("result.pending_measurements must be an array")

    return {
        "protocol": protocol,
        "version": version,
        "ok": ok,
        "exit_code": exit_code,
        "diagnostics": diagnostics,
        "truncated_diagnostics": truncated,
        "result": result,
        "pending_measurements": pending,
    }


def _validate_inject(raw: Any, task: str) -> Dict[str, Any]:
    if not isinstance(raw, dict):
        raise EnvelopeError(
            "pending measurement for task {!r} has no usable 'inject' span".format(task)
        )
    file_name = raw.get("file")
    if not isinstance(file_name, str) or not file_name:
        raise EnvelopeError(
            "pending measurement for task {!r}: inject.file must be a non-empty string".format(
                task
            )
        )
    byte_start = raw.get("byte_start")
    byte_end = raw.get("byte_end")
    if (
        not _is_int(byte_start)
        or not _is_int(byte_end)
        or byte_start < 0
        or byte_end <= byte_start
    ):
        raise EnvelopeError(
            "pending measurement for task {!r}: invalid byte span [{!r}, {!r})".format(
                task, byte_start, byte_end
            )
        )
    return {
        "file": file_name,
        "byte_start": byte_start,
        "byte_end": byte_end,
        "line": raw.get("line"),
        "column": raw.get("column"),
    }


def validate_pending_measurements(entries: Any) -> List[Dict[str, Any]]:
    """Normalize and validate ``result.pending_measurements`` entries."""
    if not isinstance(entries, list):
        raise EnvelopeError("result.pending_measurements must be an array")
    pending = []  # type: List[Dict[str, Any]]
    seen = set()  # type: set
    for index, raw in enumerate(entries):
        if not isinstance(raw, dict):
            raise EnvelopeError(
                "pending_measurements[{}] must be an object".format(index)
            )
        task = raw.get("task")
        if not isinstance(task, str) or not task:
            raise EnvelopeError(
                "pending_measurements[{}].task must be a non-empty string".format(index)
            )
        if task in seen:
            raise EnvelopeError("duplicate pending measurement for task {!r}".format(task))
        seen.add(task)

        expected = raw.get("expected_segments")
        if not _is_int(expected) or expected < 1:
            raise EnvelopeError(
                "pending measurement for task {!r}: expected_segments must be a "
                "positive integer".format(task)
            )
        runs = raw.get("runs", DEFAULT_RUNS)
        if not _is_int(runs) or runs < 1:
            raise EnvelopeError(
                "pending measurement for task {!r}: runs must be a positive integer".format(
                    task
                )
            )
        timeout = raw.get("timeout", DEFAULT_TIMEOUT_S)
        if not _is_int(timeout) or timeout < 1:
            raise EnvelopeError(
                "pending measurement for task {!r}: timeout must be a positive integer".format(
                    task
                )
            )
        pending.append(
            {
                "task": task,
                "expected_segments": expected,
                "runs": runs,
                "timeout": timeout,
                "inject": _validate_inject(raw.get("inject"), task),
            }
        )
    return pending


def render_diagnostics(envelope: Mapping[str, Any], exit_code: Optional[int] = None) -> str:
    """Render the envelope diagnostics as human-readable text."""
    code = envelope.get("exit_code", exit_code)
    diagnostics = envelope.get("diagnostics", [])
    count = len(diagnostics) if isinstance(diagnostics, list) else 0
    lines = ["ptpn check failed (exit {}): {} diagnostic(s)".format(code, count)]
    if envelope.get("truncated_diagnostics"):
        lines.append("  (diagnostics truncated)")
    if isinstance(diagnostics, list):
        for diagnostic in diagnostics:
            if not isinstance(diagnostic, dict):
                lines.append("  <malformed diagnostic>")
                continue
            header = "  {}".format(diagnostic.get("code", "<no-code>"))
            category = diagnostic.get("category")
            severity = diagnostic.get("severity")
            if category or severity:
                header += " [{}]".format(
                    "/".join(str(part) for part in (category, severity) if part)
                )
            location = diagnostic.get("location")
            if isinstance(location, dict):
                where = str(location.get("file", ""))
                if location.get("line") is not None:
                    where += ":{}".format(location["line"])
                    if location.get("column") is not None:
                        where += ":{}".format(location["column"])
                if location.get("span") is not None:
                    where += " (span {})".format(location["span"])
                if where:
                    header += " at " + where
            subject = diagnostic.get("subject")
            if subject:
                header += " subject={}".format(subject)
            lines.append(header)
            message = diagnostic.get("message")
            if message:
                lines.append("    {}".format(message))
            fix = diagnostic.get("fix")
            if fix:
                lines.append("    fix: {}".format(fix))
    return "\n".join(lines)


def run_check(ptpn_bin: Path, rtsl_path: Path) -> Tuple[int, str, str]:
    """Run ``ptpn check --diagnostics json`` and return (exit, stdout, stderr)."""
    command = [
        str(ptpn_bin),
        "check",
        "-f",
        str(rtsl_path),
        "--diagnostics",
        "json",
    ]
    try:
        process = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as exc:
        raise MeasureError("could not run ptpn check: {}".format(exc))
    return process.returncode, process.stdout, process.stderr


# ---------------------------------------------------------------------------
# Measurement plan: one synthetic proposal task per RTSL time segment
# ---------------------------------------------------------------------------


def load_task_descriptions(path: str) -> Dict[str, str]:
    """Load ``{task-id: natural-language description}`` from *path*."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        raise MeasureError("could not read tasks JSON {!r}: {}".format(path, exc))
    try:
        raw = strict_json_loads(text)
    except ValueError as exc:
        raise MeasureError(
            "tasks JSON {!r} is not valid JSON: {}".format(path, exc)
        )
    if not isinstance(raw, dict):
        raise MeasureError("tasks JSON must be an object mapping task ids to descriptions")
    descriptions = {}  # type: Dict[str, str]
    for key, value in raw.items():
        if not isinstance(key, str) or not key.strip():
            raise MeasureError("tasks JSON keys must be non-empty task ids")
        if not isinstance(value, str) or not value.strip():
            raise MeasureError(
                "tasks JSON description for {!r} must be a non-empty string".format(key)
            )
        descriptions[key.strip()] = value.strip()
    return descriptions


def segment_role(lock_count: int, index: int) -> str:
    """Describe one TDG execution segment (see docs/rule.md lock templates)."""
    if index < 0 or index > 2 * lock_count:
        raise MeasureError(
            "segment index {} is outside 0..{} for {} lock(s)".format(
                index, 2 * lock_count, lock_count
            )
        )
    if lock_count == 0:
        return "the task's total execution (the task declares no locks)"
    if index == 0:
        return "code executed before acquiring lock 1 (no locks held)"
    if index < lock_count:
        return (
            "code executed after acquiring lock {} and before acquiring lock {} "
            "(holding {} lock(s))".format(index, index + 1, index)
        )
    if index == lock_count:
        return "the critical section executed while holding all {} lock(s)".format(
            lock_count
        )
    released = 2 * lock_count - index + 1
    held = released - 1
    if held == 0:
        return "code executed after releasing lock {} (no locks held)".format(released)
    return "code executed after releasing lock {} (holding {} lock(s))".format(
        released, held
    )


def _measurement_description(
    task: str, description: str, index: int, expected_segments: int, role: str
) -> str:
    if expected_segments == 1:
        return (
            "Task '{task}': {description}\n\n"
            "This task has a single execution segment. Write a self-contained C "
            "program that performs the task's work and is measurable on its own; "
            "exercise the representative worst-case path on every run."
        ).format(task=task, description=description)
    return (
        "Task '{task}': {description}\n\n"
        "Measure segment {index} of {expected} of this task: {role}. The task "
        "declares {locks} lock(s), so the analysis models {expected} execution "
        "segments. Write a self-contained C program that performs only this "
        "segment's work and is measurable on its own; exercise the "
        "representative worst-case path on every run."
    ).format(
        task=task,
        description=description,
        index=index + 1,
        expected=expected_segments,
        role=role,
        locks=(expected_segments - 1) // 2,
    )


def plan_segments(
    pending: Sequence[Mapping[str, Any]], descriptions: Mapping[str, str]
) -> List[Dict[str, Any]]:
    """Build one proposal entry per measured segment.

    Single-segment tasks keep their real task id as the proposal name.
    Multi-segment tasks use ``<task>::seg<index>``; ``:`` cannot appear in an
    RTSL identifier, so these names cannot collide with a real task.
    """
    plan = []  # type: List[Dict[str, Any]]
    seen_names = set()  # type: set
    for entry in pending:
        task = str(entry["task"])
        description = descriptions.get(task)
        if not description:
            raise MeasureError(
                "no description for pending task {!r}; add it to --tasks-json "
                'as {{"{task}": "<what the task does>"}}'.format(task, task=task)
            )
        expected = int(entry["expected_segments"])
        lock_count = (expected - 1) // 2
        for index in range(expected):
            proposal_name = (
                task
                if expected == 1
                else "{}{}{}".format(task, _SEGMENT_NAME_SEPARATOR, index)
            )
            if proposal_name in seen_names:
                raise MeasureError(
                    "duplicate measurement proposal name {!r}".format(proposal_name)
                )
            seen_names.add(proposal_name)
            role = segment_role(lock_count, index)
            plan.append(
                {
                    "task": task,
                    "segment": index,
                    "expected_segments": expected,
                    "runs": int(entry["runs"]),
                    "timeout": int(entry["timeout"]),
                    "proposal_name": proposal_name,
                    "role": role,
                    "description": _measurement_description(
                        task, description, index, expected, role
                    ),
                }
            )
    return plan


def build_measurement_spec(plan: Sequence[Mapping[str, Any]]) -> Dict[str, Any]:
    """Build the one-CPU spec consumed by the existing prompt/validator.

    The spec is synthetic: it only gives the LLM one task per measured
    segment with a deterministic description.  Timing, dependencies, and core
    assignment do not participate in measurement.
    """
    tasks = []  # type: List[Dict[str, Any]]
    for segment in plan:
        tasks.append(
            {
                "name": segment["proposal_name"],
                "description": segment["description"],
                "period": 0,
                "deadline": 0,
                "priority": 100,
                "allowed_cores": [0],
            }
        )
    return {"num_cpus": 1, "cores_per_cpu": 1, "tasks": tasks}


def obtain_sources(spec: Any, response_file: Optional[str]) -> Dict[str, str]:
    """Return proposal name -> C source, from a provider or replay file."""
    if response_file:
        try:
            text = Path(response_file).read_text(encoding="utf-8")
        except OSError as exc:
            raise MeasureError(
                "could not read response file {!r}: {}".format(response_file, exc)
            )
        model = parse_model_text(spec, text)
    else:
        model = generate_model(spec, client_from_env())
    return dict((task["name"], task["source"]) for task in model["tasks"])


# ---------------------------------------------------------------------------
# Units, replacement formatting, and the artifact string
# ---------------------------------------------------------------------------


def observed_ns_to_ms(nanoseconds: int) -> int:
    """Convert one observed wall-clock sample to the RTSL millisecond base.

    The prototype reports integer microseconds by floor division, which loses
    sub-microsecond detail.  The RTSL loop keeps the raw nanosecond samples
    and rounds **up** (ceiling) to whole milliseconds; any non-zero
    observation is at least 1 ms.  Rounding up is the conservative direction
    for the interval's upper bound, and it never lets a measured task become
    zero-time in a ms-base model.
    """
    if not _is_int(nanoseconds):
        raise MeasureError("measured nanoseconds must be an integer")
    if nanoseconds <= 0:
        return 0
    return max(1, (nanoseconds + NS_PER_MS - 1) // NS_PER_MS)


def escape_rtsl_string(value: str) -> str:
    """Escape *value* for an RTSL double-quoted string literal."""
    if not isinstance(value, str):
        raise TypeError("RTSL string value must be a string")
    escaped = []  # type: List[str]
    for character in value:
        if character == "\\":
            escaped.append("\\\\")
        elif character == '"':
            escaped.append('\\"')
        elif character == "\n":
            escaped.append("\\n")
        elif character in "\r\t" or ord(character) < 0x20:
            escaped.append(" ")
        else:
            escaped.append(character)
    return "".join(escaped)


def format_time_attribute(
    segments: Sequence[Tuple[int, int]],
    runs: int,
    timeout: int,
    artifact: str,
) -> str:
    """Format the complete replacement for a ``time`` attribute (spec §9).

    A single segment uses ``time [a, b] measured ...``; multiple segments use
    the bracketed list form ``time [ [a, b], [c, d] ] measured ...``.
    """
    if not segments:
        raise ValueError("at least one time segment is required")
    parts = []  # type: List[str]
    for segment in segments:
        if (
            not isinstance(segment, (tuple, list))
            or len(segment) != 2
            or not _is_int(segment[0])
            or not _is_int(segment[1])
            or segment[0] < 0
            or segment[1] < segment[0]
        ):
            raise ValueError("time segments must be [min, max] with 0 <= min <= max")
        parts.append("[{}, {}]".format(segment[0], segment[1]))
    interval_text = parts[0] if len(parts) == 1 else "[ {} ]".format(", ".join(parts))
    if not _is_int(runs) or runs < 1:
        raise ValueError("runs must be a positive integer")
    if not _is_int(timeout) or timeout < 1:
        raise ValueError("timeout must be a positive integer")
    if not isinstance(artifact, str) or not artifact:
        raise ValueError("artifact must be a non-empty string")
    return 'time {} measured by llm {{ runs {}, timeout {}, artifact "{}" }}'.format(
        interval_text, runs, timeout, escape_rtsl_string(artifact)
    )


def format_time_attribute_template(expected_segments: int, runs: int, timeout: int) -> str:
    """Replacement template shown by a dry run (no measurement values yet)."""
    if not _is_int(expected_segments) or expected_segments < 1:
        raise ValueError("expected_segments must be a positive integer")
    parts = ["[<min>, <max>]"] * expected_segments
    interval_text = parts[0] if expected_segments == 1 else "[ {} ]".format(", ".join(parts))
    return (
        'time {} measured by llm {{ runs {}, timeout {}, artifact '
        '"<sha256:..., ...> cc:<compile command>" }}'
    ).format(interval_text, runs, timeout)


def format_artifact(segment_metadata: Sequence[Mapping[str, Any]]) -> str:
    """Format ``sha256:<hex>[,<hex>...] cc:<compile command>[ ; ...]``.

    One hash and one compile command are recorded per measured segment, in
    segment order.  The command strings come from the exact argument list used
    by ``measure_task`` (never a shell), joined for display with ``shlex.join``.
    """
    if not segment_metadata:
        raise ValueError("at least one segment measurement is required")
    hashes = []  # type: List[str]
    commands = []  # type: List[str]
    for metadata in segment_metadata:
        digest = metadata.get("binary_sha256")
        if not isinstance(digest, str) or not _SHA256_RE.match(digest):
            raise ValueError("binary_sha256 must be a 64-character hex digest")
        command = metadata.get("compile_command")
        if not isinstance(command, (list, tuple)) or not command:
            raise ValueError("compile_command must be a non-empty argument list")
        hashes.append(digest.lower())
        commands.append(shlex.join([str(argument) for argument in command]))
    return "sha256:{} cc:{}".format(",".join(hashes), " ; ".join(commands))


# ---------------------------------------------------------------------------
# Byte-slice injection
# ---------------------------------------------------------------------------


def _looks_like_measured_time(text: str) -> bool:
    stripped = text.lstrip()
    if not stripped.startswith("time"):
        return False
    rest = stripped[4:]
    if rest and (rest[0].isalnum() or rest[0] == "_"):
        return False
    return "measured" in rest and "llm" in rest


def collect_replacements(
    data: bytes,
    pending: Sequence[Mapping[str, Any]],
    replacements: Mapping[str, str],
) -> List[Dict[str, Any]]:
    """Bind pending entries to byte spans and verify the current source text."""
    items = []  # type: List[Dict[str, Any]]
    for entry in pending:
        task = str(entry["task"])
        injection = entry["inject"]
        start = int(injection["byte_start"])
        end = int(injection["byte_end"])
        if not (0 <= start < end <= len(data)):
            raise InjectionError(
                "inject span [{}, {}) for task {!r} is outside the {} byte input".format(
                    start, end, task, len(data)
                )
            )
        expected = data[start:end]
        try:
            expected_text = expected.decode("utf-8")
        except UnicodeDecodeError:
            raise InjectionError(
                "inject span for task {!r} is not valid UTF-8".format(task)
            )
        if not _looks_like_measured_time(expected_text):
            raise InjectionError(
                "inject span for task {!r} does not cover a measured `time` "
                "attribute (found {!r}); refusing to modify the model".format(
                    task, expected_text[:80]
                )
            )
        if task not in replacements:
            raise InjectionError(
                "no replacement was produced for pending task {!r}".format(task)
            )
        items.append(
            {
                "task": task,
                "start": start,
                "end": end,
                "expected": expected,
                "replacement": replacements[task].encode("utf-8"),
            }
        )

    ordered = sorted(items, key=lambda item: item["start"])
    for previous, current in zip(ordered, ordered[1:]):
        if current["start"] < previous["end"]:
            raise InjectionError(
                "inject spans for tasks {!r} and {!r} overlap".format(
                    previous["task"], current["task"]
                )
            )
    return items


def apply_injections(data: bytes, items: Sequence[Mapping[str, Any]]) -> bytes:
    """Replace each verified span exactly once (descending offset order).

    The span text is re-checked against the supplied bytes immediately before
    replacement; a stale span aborts the whole operation without producing a
    partially modified buffer.
    """
    if not items:
        raise InjectionError("no inject spans to apply")
    for item in items:
        start = int(item["start"])
        end = int(item["end"])
        if data[start:end] != item["expected"]:
            raise InjectionError(
                "inject span for task {!r} no longer matches the source text; "
                "aborting without modifying the model".format(item["task"])
            )
    result = bytearray(data)
    for item in sorted(items, key=lambda entry: entry["start"], reverse=True):
        result[int(item["start"]): int(item["end"])] = item["replacement"]
    return bytes(result)


def _atomic_write_bytes(path: Path, data: bytes) -> None:
    directory = path.parent if str(path.parent) else Path(".")
    directory.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".ptpn_measure_", dir=str(directory))
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
        try:
            os.chmod(temporary, stat.S_IMODE(path.stat().st_mode))
        except OSError:
            pass
        os.replace(temporary, str(path))
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _write_result_bytes(target: Path, data: bytes, in_place: bool) -> None:
    if in_place:
        _atomic_write_bytes(target, data)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _report(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def _print_error(message: str) -> None:
    print("error: {}".format(message), file=sys.stderr)


def _same_path(first: Path, second: Path) -> bool:
    try:
        return first.resolve() == second.resolve()
    except OSError:  # pragma: no cover - defensive
        return False


def _dry_run_report(
    rtsl_path: Path,
    pending: Sequence[Mapping[str, Any]],
    plan: Sequence[Mapping[str, Any]],
    prompt: str,
) -> Dict[str, Any]:
    tasks = []  # type: List[Dict[str, Any]]
    for entry in pending:
        segments = [segment for segment in plan if segment["task"] == entry["task"]]
        tasks.append(
            {
                "task": entry["task"],
                "expected_segments": entry["expected_segments"],
                "runs": entry["runs"],
                "timeout": entry["timeout"],
                "inject": entry["inject"],
                "segments": [
                    {
                        "index": segment["segment"],
                        "proposal_name": segment["proposal_name"],
                        "role": segment["role"],
                    }
                    for segment in segments
                ],
                "replacement_template": format_time_attribute_template(
                    entry["expected_segments"], entry["runs"], entry["timeout"]
                ),
            }
        )
    return {
        "status": "dry_run",
        "rtsl": str(rtsl_path),
        "pending_measurements": tasks,
        "prompt": prompt,
        "message": (
            "No files were written and no programs were compiled or executed. "
            "Rerun with --execute to measure and inject the intervals."
        ),
    }


def _run_measure(args: Any) -> int:
    rtsl_path = Path(args.rtsl)
    ptpn_bin = resolve_ptpn_bin(args.ptpn)
    ensure_ptpn_bin(ptpn_bin)

    exit_code, stdout_text, stderr_text = run_check(ptpn_bin, rtsl_path)
    try:
        envelope = parse_envelope(stdout_text)
    except EnvelopeError as exc:
        detail = [line for line in stderr_text.strip().splitlines() if line.strip()]
        suffix = " ({})".format(detail[-1]) if detail else ""
        raise EnvelopeError("{} (ptpn exit {}){}".format(exc, exit_code, suffix))

    if exit_code != 0 or not envelope["ok"]:
        sys.stderr.write(render_diagnostics(envelope, exit_code) + "\n")
        return 2

    pending = validate_pending_measurements(envelope["pending_measurements"])
    if not pending:
        _report(
            {
                "status": "nothing_to_measure",
                "rtsl": str(rtsl_path),
                "pending_measurements": [],
                "message": (
                    "No `measured by llm` task is waiting for intervals; the model "
                    "is ready for ptpn rtsl / ptpn allocate."
                ),
            }
        )
        return 0

    if not args.tasks_json:
        raise MeasureError(
            "pending measured tasks require --tasks-json PATH mapping each task id "
            "to a natural-language description"
        )
    descriptions = load_task_descriptions(args.tasks_json)
    plan = plan_segments(pending, descriptions)
    spec = build_measurement_spec(plan)
    prompt = build_prompt(spec)

    if not args.execute:
        _report(_dry_run_report(rtsl_path, pending, plan, prompt))
        return 0

    sources = obtain_sources(spec, args.response_file)

    original = rtsl_path.read_bytes()
    replacements = {}  # type: Dict[str, str]
    task_reports = []  # type: List[Dict[str, Any]]
    for entry in pending:
        task = str(entry["task"])
        segments = [segment for segment in plan if segment["task"] == task]
        metadata = []  # type: List[Dict[str, Any]]
        segments_ms = []  # type: List[Tuple[int, int]]
        segments_us = []  # type: List[Tuple[int, int]]
        for segment in segments:
            source = sources.get(segment["proposal_name"])
            if source is None:  # pragma: no cover - response validation guarantees it
                raise MeasureError(
                    "no C source proposed for segment {!r}".format(
                        segment["proposal_name"]
                    )
                )
            measured = measure_task(
                source,
                runs=int(entry["runs"]),
                timeout_s=float(entry["timeout"]),
                compiler=args.compiler,
                opt=args.opt,
                allow_execute=True,
                include_metadata=True,
            )
            metadata.append(measured)
            low_ms = observed_ns_to_ms(int(measured["min_ns"]))
            high_ms = observed_ns_to_ms(int(measured["max_ns"]))
            if high_ms < low_ms:  # pragma: no cover - ceiling is monotone
                high_ms = low_ms
            segments_ms.append((low_ms, high_ms))
            segments_us.append((int(measured["lo"]), int(measured["hi"])))

        artifact = format_artifact(metadata)
        replacement = format_time_attribute(
            segments_ms, int(entry["runs"]), int(entry["timeout"]), artifact
        )
        replacements[task] = replacement
        task_reports.append(
            {
                "task": task,
                "runs": entry["runs"],
                "timeout": entry["timeout"],
                "segments_ms": [list(segment) for segment in segments_ms],
                "segments_us": [list(segment) for segment in segments_us],
                "artifact": artifact,
                "replacement": replacement,
            }
        )

    items = collect_replacements(original, pending, replacements)
    current = rtsl_path.read_bytes()
    if current != original:
        raise InjectionError(
            "the RTSL file changed while tasks were being measured; "
            "aborting without modifying it"
        )
    injected = apply_injections(current, items)

    target = Path(args.out) if args.out else rtsl_path
    in_place = _same_path(target, rtsl_path) if args.out else True
    _write_result_bytes(target, injected, in_place=in_place)

    try:
        recheck_code, recheck_stdout, _ = run_check(ptpn_bin, target)
        recheck = parse_envelope(recheck_stdout)
    except MeasureError as exc:
        _report(
            {
                "status": "recheck_failed",
                "input": str(rtsl_path),
                "out": str(target),
                "error": str(exc),
                "tasks": task_reports,
                "note": (
                    "The file was written, but the post-injection check could not "
                    "be interpreted. Inspect it before continuing."
                ),
            }
        )
        return 2

    remaining = []  # type: List[str]
    for entry in recheck["pending_measurements"]:
        if isinstance(entry, dict) and isinstance(entry.get("task"), str):
            remaining.append(entry["task"])

    if recheck_code != 0 or not recheck["ok"]:
        sys.stderr.write(render_diagnostics(recheck, recheck_code) + "\n")
        _report(
            {
                "status": "recheck_failed",
                "input": str(rtsl_path),
                "out": str(target),
                "tasks": task_reports,
                "recheck": {
                    "exit_code": recheck_code,
                    "ok": False,
                    "diagnostics": recheck["diagnostics"],
                    "pending_measurements": remaining,
                },
                "note": (
                    "The file was written, but the post-injection check failed. "
                    "Inspect the diagnostics before continuing."
                ),
            }
        )
        return 2

    status = "recheck_incomplete" if remaining else "ok"
    _report(
        {
            "status": status,
            "input": str(rtsl_path),
            "out": str(target) if not in_place else None,
            "in_place": in_place,
            "unit": "milliseconds",
            "conversion": (
                "wall-clock nanoseconds rounded up (ceiling) to whole "
                "milliseconds; a non-zero observation is at least 1 ms"
            ),
            "wcet_kind": "measured",
            "wcet_method": DEFAULT_WCET_METHOD,
            "tasks": task_reports,
            "recheck": {
                "exit_code": recheck_code,
                "ok": True,
                "diagnostics": recheck["diagnostics"],
                "truncated_diagnostics": recheck["truncated_diagnostics"],
                "pending_measurements": remaining,
            },
            "note": (
                "Measured wall-clock intervals are observations, not a WCET upper "
                "bound and not suitable for hard real-time claims."
            ),
        }
    )
    return 2 if remaining else 0


def run_measure_command(args: Any) -> int:
    """CLI entry point for ``python -m tools.llm measure``."""
    try:
        return _run_measure(args)
    except MeasurementError as exc:
        print(
            json.dumps(
                {"status": "error", "error": exc.to_dict()},
                indent=2,
                sort_keys=True,
            ),
            file=sys.stderr,
        )
        return 3
    except SchemaError as exc:
        for message in exc.errors:
            _print_error(message)
        return 2
    except MeasureError as exc:
        _print_error(str(exc))
        return 2
    except LLMError as exc:
        _print_error(str(exc))
        return 2
    except (OSError, TypeError, ValueError) as exc:
        _print_error(str(exc))
        return 2
