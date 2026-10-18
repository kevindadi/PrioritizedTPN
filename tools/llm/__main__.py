"""Command-line entry point for the PTPN LLM front-end prototype.

Examples::

    python -m tools.llm generate --spec spec.json --out model.json --dry-run
    python -m tools.llm generate --spec spec.json --out model.json
    python -m tools.llm generate --spec spec.json --out model.json --execute
    python -m tools.llm generate --spec spec.json --response-file response.txt \\
        --out model.json --execute --tdg-out graph.json
"""

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, Optional, Sequence

from .client import LLMError, client_from_env, strict_json_loads
from .pipeline import (
    DEFAULT_WCET_METHOD,
    MeasurementError,
    build_prompt,
    emit_tdg,
    generate_model,
    measure_task,
    parse_model_text,
)
from .schema import SchemaError, validate_spec


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.llm",
        description="Generate and validate task models with an untrusted LLM proposer.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser(
        "generate",
        help="ask a provider (or replay a response file) for a task model",
    )
    generate.add_argument("--spec", required=True, help="path to the task specification JSON")
    generate.add_argument("--out", required=True, help="path for the validated model JSON")
    generate.add_argument(
        "--execute",
        action="store_true",
        help="compile and run generated C programs, then emit a measured TDG",
    )
    generate.add_argument(
        "--runs", type=int, default=20, help="timed executions per task (default: 20)"
    )
    generate.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="wall-clock timeout per execution in seconds (default: 5)",
    )
    generate.add_argument(
        "--dry-run",
        action="store_true",
        help="print the deterministic prompt and exit without calling a provider",
    )
    generate.add_argument(
        "--response-file",
        help="replay a saved LLM response instead of calling a provider (offline mode)",
    )
    generate.add_argument(
        "--measured-json",
        help=(
            "JSON object mapping task names to measured intervals or per-segment "
            "interval lists; supplied tasks are not executed"
        ),
    )
    generate.add_argument(
        "--tdg-out",
        help="TDG output path when --execute is used (default: <out>.tdg.json)",
    )
    generate.add_argument("--compiler", default="cc", help="C compiler executable (default: cc)")
    generate.add_argument("--opt", default="-O2", help="optimisation flag (default: -O2)")
    return parser


def _read_text(path: str) -> str:
    return Path(path).read_text(encoding="utf-8")


def _load_json_file(path: str) -> Any:
    return strict_json_loads(_read_text(path))


def _write_json(path: str, value: Any) -> None:
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def _default_tdg_path(out_path: str) -> str:
    destination = Path(out_path)
    try:
        return str(destination.with_suffix(".tdg.json"))
    except ValueError:  # pragma: no cover - unusual name such as "."
        return str(destination) + ".tdg.json"


def _print_error(message: str) -> None:
    print("error: {}".format(message), file=sys.stderr)


def _print_schema_error(exc: SchemaError) -> None:
    for message in exc.errors:
        _print_error(message)


def _report(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    try:
        raw_spec = _load_json_file(args.spec)
    except (OSError, ValueError) as exc:
        _print_error("could not read spec {!r}: {}".format(args.spec, exc))
        return 2
    try:
        spec = validate_spec(raw_spec)
    except SchemaError as exc:
        _print_schema_error(exc)
        return 2

    if args.dry_run:
        print(build_prompt(spec))
        return 0

    supplied_measurements = {}  # type: Dict[str, Any]
    if args.measured_json:
        try:
            loaded = _load_json_file(args.measured_json)
        except (OSError, ValueError) as exc:
            _print_error(
                "could not read measured intervals {!r}: {}".format(
                    args.measured_json, exc
                )
            )
            return 2
        if not isinstance(loaded, dict):
            _print_error("--measured-json must contain an object mapping task names to intervals")
            return 2
        supplied_measurements = loaded

    should_emit_tdg = args.tdg_out is not None or args.measured_json is not None
    if args.tdg_out and not args.execute and not supplied_measurements:
        _print_error(
            "refusing to emit a TDG without measured timing; pass --execute "
            "or --measured-json, or omit --tdg-out"
        )
        return 2

    try:
        if args.response_file:
            model = parse_model_text(spec, _read_text(args.response_file))
        else:
            model = generate_model(spec, client_from_env())
    except SchemaError as exc:
        _print_schema_error(exc)
        return 2
    except (OSError, ValueError, LLMError, TypeError) as exc:
        _print_error(str(exc))
        return 2

    try:
        _write_json(args.out, model)
    except OSError as exc:
        _print_error("could not write model to {!r}: {}".format(args.out, exc))
        return 2

    task_names = [task["name"] for task in spec["tasks"]]
    measured = dict(supplied_measurements)  # type: Dict[str, Any]
    missing = [name for name in task_names if name not in measured]

    if missing and not args.execute:
        if should_emit_tdg:
            _print_error(
                "no measurement for {}; pass --execute or add the tasks to "
                "--measured-json".format(", ".join(missing))
            )
            return 2
        _report(
            {
                "status": "needs_measurement",
                "model_out": args.out,
                "tdg_out": None,
                "tasks": task_names,
                "missing": missing,
                "message": (
                    "Model written but no TDG emitted: execution timing was not "
                    "measured. Rerun with --execute to compile and run each task, "
                    "or supply --measured-json."
                ),
            }
        )
        return 0

    sources = dict((task["name"], task["source"]) for task in model["tasks"])
    for name in missing:
        try:
            measured[name] = measure_task(
                sources[name],
                runs=args.runs,
                timeout_s=args.timeout,
                compiler=args.compiler,
                opt=args.opt,
                allow_execute=True,
            )
        except MeasurementError as exc:
            print(
                json.dumps(
                    {"status": "error", "task": name, "error": exc.to_dict()},
                    indent=2,
                    sort_keys=True,
                ),
                file=sys.stderr,
            )
            return 3

    try:
        tdg = emit_tdg(spec, model, measured)
    except SchemaError as exc:
        _print_schema_error(exc)
        return 2

    tdg_path = args.tdg_out or _default_tdg_path(args.out)
    try:
        _write_json(tdg_path, tdg)
    except OSError as exc:
        _print_error("could not write TDG to {!r}: {}".format(tdg_path, exc))
        return 2

    _report(
        {
            "status": "ok",
            "model_out": args.out,
            "tdg_out": tdg_path,
            "runs": args.runs,
            "timeout_s": args.timeout,
            "unit": "microseconds",
            "wcet_kind": "measured",
            "wcet_method": DEFAULT_WCET_METHOD,
            "measured": measured,
            "note": (
                "Measured wall-clock intervals are observations, not a WCET "
                "upper bound and not suitable for hard real-time claims."
            ),
        }
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
