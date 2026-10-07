#!/usr/bin/env python3
"""Run all PTPN experiments: p/s/t-bench exports plus l-bench scaling.

Benchmark suites (example/<suite>/*.json) run through the export profiles:
  ptpn    -> example/ptpn/<suite>/<case>.{ptpn,scg}.dot
  romeo   -> example/romeo/<suite>/<case>.cts
  ptopner -> example/ptopner/<suite>/<case>.ppn   (s-bench skipped)
Summary: example/bench-summary.json and .csv

l-bench (example/l-bench/pipeline-*.json, ordered by manifest.json) runs PTPN
lowering plus SCG analysis and records scaling metrics.
Summary: example/l-bench/bench-summary.json and .csv

Examples:
  python3 run.py                              # every suite + every l-bench case
  python3 run.py --skip-lbench
  python3 run.py --suites p-bench t-bench --profiles ptpn
  python3 run.py --lbench-cases pipeline-40t-8c-5l -m 20000
  python3 run.py --reviewer-case
  python3 run.py --dry-run
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_BUILD_DIR = ROOT / "build"
BUILD_DIR_MARKER = ROOT / ".ptpn-build-dir"
PTPN_EXE = "ptpn.exe" if os.name == "nt" else "ptpn"

EXAMPLE = ROOT / "example"
BENCH_SUITES = ("p-bench", "s-bench", "t-bench")
SKIP_PTOPNER_SUITES = frozenset({"s-bench"})

LBENCH_DIR = EXAMPLE / "l-bench"
PIPELINE_GLOB = "pipeline-*.json"

RE_PLACES = re.compile(r"Places:\s*(\d+)")
RE_TRANSITIONS = re.compile(r"Transitions:\s*(\d+)")
RE_STATS_TDG2PN = re.compile(r"\[STATS\] TDG2PN lowering:\s*(\d+)\s*ms")
RE_STATS_SCG = re.compile(
    r"\[STATS\] SCG build:\s*(\d+)\s*ms "
    r"\(states=(\d+), edges=(\d+), dedup_hits=(\d+)(, truncated)?\)"
)
RE_STATS_SCG_DOT = re.compile(r"\[STATS\] SCG DOT export:\s*(\d+)\s*ms")
RE_STATS_TOTAL = re.compile(
    r"\[STATS\] TDG pipeline \(total\):\s*(\d+)\s*ms(?: \((\d+) KB\))?"
)
RE_STATS_PTPN_TOTAL = re.compile(
    r"\[STATS\] PTPN pipeline \(total\):\s*(\d+)\s*ms(?: \((\d+) KB\))?"
)
RE_BUILD_TRUNCATED = re.compile(r"build complete:.*truncated=(true|false)")
RE_TDG_DOT = re.compile(r"\[DOT\] Exported (\d+) nodes, (\d+) edges")
RE_MEMORY_KB = re.compile(r"\((\d+) KB\)")


# ---------------------------------------------------------------------------
# Build directory / binary resolution
# ---------------------------------------------------------------------------


def read_build_dir() -> Path:
    env = os.environ.get("PTPN_BUILD_DIR", "").strip()
    if env:
        path = Path(env).expanduser()
        return (ROOT / path).resolve() if not path.is_absolute() else path.resolve()

    if BUILD_DIR_MARKER.is_file():
        line = BUILD_DIR_MARKER.read_text(encoding="utf-8").strip()
        if line:
            path = Path(line).expanduser()
            return (ROOT / path).resolve() if not path.is_absolute() else path.resolve()

    return DEFAULT_BUILD_DIR.resolve()


def resolve_ptpn_bin(explicit: Path | None = None) -> Path:
    if explicit is not None:
        path = explicit.expanduser()
        return (ROOT / path).resolve() if not path.is_absolute() else path.resolve()
    return read_build_dir() / PTPN_EXE


def default_ptpn_help() -> str:
    build_dir = read_build_dir()
    return (
        f"{build_dir / PTPN_EXE} (from {build_dir.name}/; "
        f"override with PTPN_BUILD_DIR or --ptpn)"
    )


def ensure_ptpn(ptpn_bin: Path) -> None:
    if ptpn_bin.is_file():
        return
    print(f"error: ptpn not found: {ptpn_bin}", file=sys.stderr)
    print("build first: ./build.py", file=sys.stderr)
    print("or set PTPN_BUILD_DIR / pass --ptpn PATH", file=sys.stderr)
    raise SystemExit(1)


# ---------------------------------------------------------------------------
# Benchmark suites (p-bench / s-bench / t-bench)
# ---------------------------------------------------------------------------


@dataclass
class RunStats:
    suite: str
    case: str
    profile: str
    ok: bool
    error: str = ""
    places: int | None = None
    transitions: int | None = None
    scg_states: int | None = None
    scg_edges: int | None = None
    scg_dedup_hits: int | None = None
    scg_truncated: bool = False
    ms_tdg2pn: int | None = None
    ms_scg_build: int | None = None
    ms_scg_dot: int | None = None
    ms_total: int | None = None
    ppn_exported: bool = False
    ppn_error: str = ""
    outputs: list[str] = field(default_factory=list)


PROFILES: dict[str, dict] = {
    "ptpn": {
        "mode": "tdg_dots",
        "policy": None,
    },
    "romeo": {
        "mode": "export_cts",
        "format": "scheduling-net",
    },
    "ptopner": {
        "mode": "export_ppn",
        "policy": "fixed_prior_with_restart",
    },
}


def discover_suite_cases(suites: list[str]) -> list[tuple[str, Path]]:
    cases: list[tuple[str, Path]] = []
    for suite in suites:
        suite_dir = EXAMPLE / suite
        if not suite_dir.is_dir():
            print(f"warning: missing suite directory {suite_dir}", file=sys.stderr)
            continue
        for path in sorted(suite_dir.glob("*.json")):
            cases.append((suite, path))
    return cases


def parse_suite_log(text: str) -> dict:
    stats: dict = {"scg_truncated": ", truncated" in text}

    if m := RE_PLACES.search(text):
        stats["places"] = int(m.group(1))
    if m := RE_TRANSITIONS.search(text):
        stats["transitions"] = int(m.group(1))
    if m := RE_STATS_TDG2PN.search(text):
        stats["ms_tdg2pn"] = int(m.group(1))
    if m := RE_STATS_SCG.search(text):
        stats["ms_scg_build"] = int(m.group(1))
        stats["scg_states"] = int(m.group(2))
        stats["scg_edges"] = int(m.group(3))
        stats["scg_dedup_hits"] = int(m.group(4))
    if m := RE_STATS_SCG_DOT.search(text):
        stats["ms_scg_dot"] = int(m.group(1))
    if m := RE_STATS_TOTAL.search(text):
        stats["ms_total"] = int(m.group(1))

    return stats


def run_ptpn_capture(cmd: list[str]) -> tuple[int, str]:
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    return proc.returncode, proc.stdout + proc.stderr


def run_suite_case(
    ptpn_bin: Path,
    suite: str,
    input_path: Path,
    profile: str,
    out_dir: Path,
    max_states: int,
) -> RunStats:
    case = input_path.stem
    cfg = PROFILES[profile]
    out_dir.mkdir(parents=True, exist_ok=True)

    result = RunStats(suite=suite, case=case, profile=profile, ok=False)

    if cfg["mode"] == "tdg_dots":
        ptpn_dot = out_dir / f"{case}.ptpn.dot"
        scg_dot = out_dir / f"{case}.scg.dot"
        cmd = [
            str(ptpn_bin),
            "tdg",
            "-f",
            str(input_path),
            "-m",
            str(max_states),
            "--export-ptpn",
            str(ptpn_dot),
            "--export-scg",
            str(scg_dot),
        ]
        try:
            code, log = run_ptpn_capture(cmd)
        except OSError as exc:
            result.error = str(exc)
            return result

        for key, value in parse_suite_log(log).items():
            setattr(result, key, value)
        result.outputs = [str(ptpn_dot), str(scg_dot)]

        if code != 0:
            result.error = log.strip()[:500]
            return result
        missing = [p for p in (ptpn_dot, scg_dot) if not p.exists()]
        if missing:
            result.error = f"missing output: {', '.join(str(p) for p in missing)}"
            return result

    elif cfg["mode"] == "export_cts":
        cts_file = out_dir / f"{case}.cts"
        cmd = [
            str(ptpn_bin),
            "export",
            "romeo",
            "-f",
            str(input_path),
            "-o",
            str(cts_file),
            "--format",
            cfg["format"],
        ]
        try:
            code, log = run_ptpn_capture(cmd)
        except OSError as exc:
            result.error = str(exc)
            return result

        for key, value in parse_suite_log(log).items():
            setattr(result, key, value)
        result.outputs = [str(cts_file)]

        if code != 0:
            result.error = log.strip()[:500]
            return result
        if not cts_file.exists():
            result.error = f"missing output: {cts_file}"
            return result

    elif cfg["mode"] == "export_ppn":
        ppn_file = out_dir / f"{case}.ppn"
        cmd = [
            str(ptpn_bin),
            "export",
            "ptopner",
            "-f",
            str(input_path),
            "-o",
            str(ppn_file),
            "--policy",
            cfg["policy"],
        ]
        try:
            code, log = run_ptpn_capture(cmd)
        except OSError as exc:
            result.error = str(exc)
            return result

        for key, value in parse_suite_log(log).items():
            setattr(result, key, value)
        result.outputs = [str(ppn_file)]

        if code != 0:
            err = log.strip()
            result.ppn_error = err.splitlines()[0][:200] if err else "export failed"
            result.error = err[:500]
            return result
        if not ppn_file.exists():
            result.error = f"missing output: {ppn_file}"
            result.ppn_error = result.error
            return result
        result.ppn_exported = True

    else:
        result.error = f"unknown profile mode: {cfg['mode']}"
        return result

    result.ok = True
    return result


def should_run_profile(profile: str, suite: str) -> bool:
    return not (profile == "ptopner" and suite in SKIP_PTOPNER_SUITES)


def write_suite_summary(rows: list[RunStats], json_path: Path, csv_path: Path) -> None:
    json_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "profiles": list(PROFILES.keys()),
        "suites": list(BENCH_SUITES),
        "skip_ptopner_suites": sorted(SKIP_PTOPNER_SUITES),
        "runs": [asdict(r) for r in rows],
    }
    json_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    fieldnames = [
        "profile",
        "suite",
        "case",
        "ok",
        "places",
        "transitions",
        "scg_states",
        "scg_edges",
        "scg_dedup_hits",
        "scg_truncated",
        "ms_tdg2pn",
        "ms_scg_build",
        "ms_scg_dot",
        "ms_total",
        "ppn_exported",
        "ppn_error",
        "error",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            data = asdict(row)
            writer.writerow({k: data.get(k) for k in fieldnames})


def print_suite_table(rows: list[RunStats]) -> None:
    header = (
        f"{'profile':<9} {'suite':<9} {'case':<22} {'ok':<4} "
        f"{'P/T':<11} {'SCG s/e':<14} {'ms(scg/tot)':<14}"
    )
    print(header)
    print("-" * len(header))
    for r in rows:
        pt = (
            f"{r.places}/{r.transitions}"
            if r.places is not None and r.transitions is not None
            else "-"
        )
        scg = (
            f"{r.scg_states}/{r.scg_edges}"
            if r.scg_states is not None and r.scg_edges is not None
            else "-"
        )
        ms = (
            f"{r.ms_scg_build or '-':>4}/{r.ms_total or '-':<4}"
            if r.ok
            else r.error[:12]
        )
        print(
            f"{r.profile:<9} {r.suite:<9} {r.case:<22} "
            f"{'yes' if r.ok else 'no':<4} {pt:<11} {scg:<14} {ms:<14}"
        )


def run_suites(ptpn_bin: Path, args: argparse.Namespace) -> int:
    cases = discover_suite_cases(args.suites)
    if not cases:
        print("error: no benchmark JSON files found", file=sys.stderr)
        return 1

    if args.dry_run:
        for suite, input_path in cases:
            for profile in args.profiles:
                if should_run_profile(profile, suite):
                    print(f"[dry-run] suite {profile}/{suite}/{input_path.stem}")
        return 0

    rows: list[RunStats] = []
    skipped = 0

    for suite, input_path in cases:
        case = input_path.stem
        for profile in args.profiles:
            if not should_run_profile(profile, suite):
                skipped += 1
                continue

            out_dir = EXAMPLE / profile / suite
            print(f"[*] {profile}/{suite}/{case} ...", flush=True)
            row = run_suite_case(
                ptpn_bin, suite, input_path, profile, out_dir, args.max_states
            )
            rows.append(row)
            print(f"    {'ok' if row.ok else 'FAILED: ' + row.error}")

    summary_json = EXAMPLE / "bench-summary.json"
    summary_csv = EXAMPLE / "bench-summary.csv"
    write_suite_summary(rows, summary_json, summary_csv)

    print()
    print_suite_table(rows)
    print()
    print(f"summary: {summary_json}")
    print(f"summary: {summary_csv}")
    if skipped:
        print(f"note: skipped {skipped} ptopner run(s) for {sorted(SKIP_PTOPNER_SUITES)}")

    failed = sum(1 for r in rows if not r.ok)
    ppn_failed = sum(1 for r in rows if r.profile == "ptopner" and not r.ppn_exported)
    if ppn_failed:
        print(f"note: {ppn_failed} ptopner run(s) failed to export .ppn")
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# l-bench (multi-lane pipeline scaling)
# ---------------------------------------------------------------------------


@dataclass
class BenchRow:
    case: str
    file: str
    tasks: int
    cpus: int
    locks: int
    lanes: int = 0
    periodic_tasks: int = 0
    fork_nodes: int = 0
    join_nodes: int = 0
    reviewer_case: bool = False
    max_states: int = 0
    canonicalization: str = "equality"
    extrapolation: bool = False
    ok: bool = False
    error: str = ""
    tdg_nodes: int | None = None
    tdg_edges: int | None = None
    places: int | None = None
    transitions: int | None = None
    scg_states: int | None = None
    scg_edges: int | None = None
    scg_dedup_hits: int | None = None
    scg_truncated: bool = False
    ms_tdg2pn: int | None = None
    ms_scg_build: int | None = None
    ms_total: int | None = None
    memory_kb: int | None = None
    validate_tdg_only: bool = False
    outputs: list[str] = field(default_factory=list)


def load_manifest(lbench_dir: Path) -> dict:
    manifest_path = lbench_dir / "manifest.json"
    if not manifest_path.is_file():
        return {}
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def manifest_case_map(manifest: dict) -> dict[str, dict]:
    return {c["filename"]: c for c in manifest.get("cases", [])}


def lbench_case_sort_key(path: Path, case_meta: dict[str, dict]) -> tuple:
    meta = case_meta.get(path.name, {})
    return (
        meta.get("num_tasks", 9999),
        meta.get("reviewer_case", False),
        path.name,
    )


def discover_lbench_cases(
    lbench_dir: Path,
    case_filter: list[str] | None,
    *,
    use_manifest: bool,
) -> list[Path]:
    case_meta = manifest_case_map(load_manifest(lbench_dir))
    all_paths = {p.name: p for p in lbench_dir.glob(PIPELINE_GLOB)}

    if use_manifest and case_meta:
        ordered = [all_paths[name] for name in case_meta if name in all_paths]
    else:
        ordered = sorted(all_paths.values(), key=lambda p: lbench_case_sort_key(p, case_meta))

    if not case_filter:
        return ordered

    wanted = set(case_filter)
    out: list[Path] = []
    for path in ordered:
        stem = path.stem
        short = stem.removeprefix("pipeline-")
        if stem in wanted or short in wanted or path.name in wanted:
            out.append(path)
    return out


def analyze_json(path: Path) -> dict:
    doc = json.loads(path.read_text(encoding="utf-8"))
    cfg = doc.get("configuration", {})
    nodes = doc.get("nodes", [])
    edges = doc.get("edges", [])

    tasks = sum(1 for n in nodes if n.get("type") == "task")
    forks = sum(1 for n in nodes if n.get("type") == "fork")
    joins = sum(1 for n in nodes if n.get("type") == "join")

    return {
        "tasks": tasks,
        "cpus": cfg.get("num_cpus", 0),
        "locks": len(cfg.get("shared_locks", [])),
        "periodic_tasks": len(cfg.get("periodic", [])),
        "fork_nodes": forks,
        "join_nodes": joins,
        "tdg_nodes": len(nodes),
        "tdg_edges": len(edges),
    }


def apply_manifest_meta(row: BenchRow, meta: dict | None) -> None:
    if not meta:
        return
    row.lanes = meta.get("num_lanes", row.lanes)
    if "periodic_tasks" in meta:
        row.periodic_tasks = meta["periodic_tasks"]
    row.reviewer_case = bool(meta.get("reviewer_case", False))


def parse_lbench_log(text: str) -> dict:
    stats: dict = {"scg_truncated": False}
    if m := RE_BUILD_TRUNCATED.search(text):
        stats["scg_truncated"] = m.group(1) == "true"
    elif "Reachability graph truncated" in text:
        stats["scg_truncated"] = True

    if m := RE_PLACES.search(text):
        stats["places"] = int(m.group(1))
    if m := RE_TRANSITIONS.search(text):
        stats["transitions"] = int(m.group(1))
    if m := RE_STATS_TDG2PN.search(text):
        stats["ms_tdg2pn"] = int(m.group(1))
    if m := RE_STATS_SCG.search(text):
        stats["ms_scg_build"] = int(m.group(1))
        stats["scg_states"] = int(m.group(2))
        stats["scg_edges"] = int(m.group(3))
        stats["scg_dedup_hits"] = int(m.group(4))
    if m := RE_TDG_DOT.search(text):
        stats["tdg_nodes"] = int(m.group(1))
        stats["tdg_edges"] = int(m.group(2))
    for pattern in (RE_STATS_TOTAL, RE_STATS_PTPN_TOTAL):
        if m := pattern.search(text):
            stats["ms_total"] = int(m.group(1))
            if m.group(2):
                stats["memory_kb"] = int(m.group(2))
            break
    if "memory_kb" not in stats:
        kb_vals = [int(m.group(1)) for m in RE_MEMORY_KB.finditer(text)]
        if kb_vals:
            stats["memory_kb"] = max(kb_vals)
    return stats


def recommended_max_states(tasks: int, periodic_tasks: int) -> int:
    base = 10_000
    if tasks >= 100:
        base = 50_000
    elif tasks >= 60:
        base = 30_000
    elif tasks >= 40:
        base = 20_000
    if periodic_tasks >= 2:
        base = int(base * 1.5)
    return base


def run_lbench_case(
    ptpn_bin: Path,
    input_path: Path,
    max_states: int,
    validate_tdg_only: bool,
    out_dir: Path,
    case_meta: dict | None,
    canonicalization: str = "equality",
    extrapolation: bool = False,
) -> BenchRow:
    case = input_path.stem
    parsed_json = analyze_json(input_path)
    row = BenchRow(
        case=case,
        file=input_path.name,
        tasks=parsed_json["tasks"],
        cpus=parsed_json["cpus"],
        locks=parsed_json["locks"],
        periodic_tasks=parsed_json["periodic_tasks"],
        fork_nodes=parsed_json["fork_nodes"],
        join_nodes=parsed_json["join_nodes"],
        tdg_nodes=parsed_json["tdg_nodes"],
        tdg_edges=parsed_json["tdg_edges"],
        max_states=max_states,
        canonicalization=canonicalization,
        extrapolation=extrapolation,
        validate_tdg_only=validate_tdg_only,
    )
    apply_manifest_meta(row, case_meta)

    out_dir.mkdir(parents=True, exist_ok=True)

    if validate_tdg_only:
        dot_path = out_dir / f"{case}.tdg.dot"
        cmd = [
            str(ptpn_bin),
            "tdg",
            "-f",
            str(input_path),
            "--export-tdg",
            str(dot_path),
            "--no-analysis",
        ]
        row.outputs = [str(dot_path)]
    else:
        scg_dot = out_dir / f"{case}.scg.dot"
        ptpn_dot = out_dir / f"{case}.ptpn.dot"
        cmd = [
            str(ptpn_bin),
            "tdg",
            "-f",
            str(input_path),
            "-m",
            str(max_states),
            "--canonicalization",
            canonicalization,
            "--export-ptpn",
            str(ptpn_dot),
            "--export-scg",
            str(scg_dot),
        ]
        if extrapolation:
            cmd.append("--extrapolation")
        row.outputs = [str(ptpn_dot), str(scg_dot)]

    try:
        code, log = run_ptpn_capture(cmd)
    except OSError as exc:
        row.error = str(exc)
        return row

    for key, value in parse_lbench_log(log).items():
        setattr(row, key, value)

    if code != 0:
        row.error = log.strip()[:500]
        return row

    missing = [p for p in row.outputs if not Path(p).is_file()]
    if missing:
        row.error = f"missing output: {', '.join(missing)}"
        return row

    row.ok = True
    return row


def write_lbench_summary(
    rows: list[BenchRow],
    json_path: Path,
    csv_path: Path,
    manifest: dict,
) -> None:
    payload = {
        "suite": "l-bench",
        "topology": manifest.get("topology"),
        "description": manifest.get("description"),
        "task_place_capacity": manifest.get("task_place_capacity"),
        "runs": [asdict(r) for r in rows],
    }
    json_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    fieldnames = [
        "case",
        "file",
        "tasks",
        "cpus",
        "locks",
        "lanes",
        "periodic_tasks",
        "fork_nodes",
        "join_nodes",
        "reviewer_case",
        "max_states",
        "canonicalization",
        "extrapolation",
        "ok",
        "tdg_nodes",
        "tdg_edges",
        "places",
        "transitions",
        "scg_states",
        "scg_edges",
        "scg_truncated",
        "memory_kb",
        "ms_tdg2pn",
        "ms_scg_build",
        "ms_total",
        "validate_tdg_only",
        "error",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            data = asdict(row)
            writer.writerow({k: data.get(k) for k in fieldnames})


def print_lbench_table(rows: list[BenchRow]) -> None:
    header = (
        f"{'case':<26} {'ok':<4} {'T/C/L':<10} {'lane/p':<8} "
        f"{'P/T':<11} {'SCG':<8} {'mem':<8} {'ms':<8}"
    )
    print(header)
    print("-" * len(header))
    for r in rows:
        tcl = f"{r.tasks}/{r.cpus}/{r.locks}"
        lp = f"{r.lanes}/{r.periodic_tasks}"
        pt = (
            f"{r.places}/{r.transitions}"
            if r.places is not None and r.transitions is not None
            else "-"
        )
        scg = str(r.scg_states) if r.scg_states is not None else "-"
        mem = str(r.memory_kb) if r.memory_kb is not None else "-"
        ms = str(r.ms_total) if r.ms_total is not None else "-"
        ok = "yes" if r.ok else "no"
        print(f"{r.case:<26} {ok:<4} {tcl:<10} {lp:<8} {pt:<11} {scg:<8} {mem:<8} {ms:<8}")


def print_state_hints(
    cases: list[Path],
    case_meta: dict[str, dict],
    max_states: int,
    *,
    validate_tdg_only: bool,
) -> None:
    if validate_tdg_only:
        return
    for path in cases:
        meta = case_meta.get(path.name, {})
        tasks = meta.get("num_tasks")
        periodic = meta.get("periodic_tasks", 0)
        if tasks is None:
            continue
        suggested = recommended_max_states(tasks, periodic)
        if max_states < suggested:
            print(
                f"note: {path.name} (tasks={tasks}, periodic={periodic}) "
                f"may need -m {suggested} or higher",
                file=sys.stderr,
            )


def run_lbench(ptpn_bin: Path, args: argparse.Namespace) -> int:
    lbench_dir = args.lbench_dir.expanduser()
    if not lbench_dir.is_absolute():
        lbench_dir = (ROOT / lbench_dir).resolve()

    manifest = load_manifest(lbench_dir)
    case_meta = manifest_case_map(manifest)

    if args.reviewer_case:
        reviewer_files = [
            name for name, meta in case_meta.items() if meta.get("reviewer_case")
        ]
        if not reviewer_files:
            print("error: no reviewer_case in manifest.json", file=sys.stderr)
            return 1
        case_filter = reviewer_files
        use_manifest = True
    else:
        case_filter = args.lbench_cases
        use_manifest = bool(case_meta)

    cases = discover_lbench_cases(lbench_dir, case_filter, use_manifest=use_manifest)
    if not cases:
        print("error: no pipeline JSON files found", file=sys.stderr)
        return 1

    if args.dry_run:
        mode = (
            "tdg-dot-only"
            if args.validate_tdg_only
            else f"full scg -m {args.max_states}"
        )
        for path in cases:
            meta = case_meta.get(path.name, {})
            lanes = meta.get("num_lanes", "?")
            periodic = meta.get("periodic_tasks", "?")
            print(f"[dry-run] l-bench {path.name} lanes={lanes} periodic={periodic} ({mode})")
        return 0

    print_state_hints(
        cases,
        case_meta,
        args.max_states,
        validate_tdg_only=args.validate_tdg_only,
    )

    out_dir = args.lbench_out_dir.expanduser()
    if not out_dir.is_absolute():
        out_dir = (ROOT / out_dir).resolve()

    rows: list[BenchRow] = []
    for path in cases:
        print(f"[*] l-bench/{path.name} ...", flush=True)
        row = run_lbench_case(
            ptpn_bin,
            path,
            args.max_states,
            args.validate_tdg_only,
            out_dir,
            case_meta.get(path.name),
            canonicalization=args.canonicalization,
            extrapolation=args.extrapolation,
        )
        rows.append(row)
        print(f"    {'ok' if row.ok else 'FAILED: ' + row.error}")

    summary_json = lbench_dir / "bench-summary.json"
    summary_csv = lbench_dir / "bench-summary.csv"
    write_lbench_summary(rows, summary_json, summary_csv, manifest)

    print()
    print_lbench_table(rows)
    print()
    print(f"summary: {summary_json}")
    print(f"summary: {summary_csv}")

    failed = sum(1 for r in rows if not r.ok)
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ptpn",
        type=Path,
        default=None,
        help=f"path to ptpn executable (default: {default_ptpn_help()})",
    )
    parser.add_argument(
        "-m",
        "--max-states",
        type=int,
        default=10_000,
        help="state-class reachability cap (default: 10000)",
    )
    parser.add_argument(
        "--suites",
        nargs="+",
        choices=BENCH_SUITES,
        default=list(BENCH_SUITES),
        help="benchmark suites to run (default: all)",
    )
    parser.add_argument(
        "--profiles",
        nargs="+",
        choices=list(PROFILES.keys()),
        default=list(PROFILES.keys()),
        help="export profiles to run (default: ptpn romeo ptopner)",
    )
    parser.add_argument(
        "--skip-suites",
        action="store_true",
        help="skip the p/s/t-bench suites",
    )
    parser.add_argument(
        "--skip-lbench",
        action="store_true",
        help="skip the l-bench pipeline cases",
    )
    parser.add_argument(
        "--lbench-dir",
        type=Path,
        default=LBENCH_DIR,
        help=f"l-bench directory (default: {LBENCH_DIR})",
    )
    parser.add_argument(
        "--lbench-cases",
        nargs="+",
        help="l-bench case stems to run, e.g. pipeline-40t-8c-5l or 40t-8c-5l",
    )
    parser.add_argument(
        "--lbench-out-dir",
        type=Path,
        default=LBENCH_DIR / "results",
        help="l-bench artifact output directory (default: example/l-bench/results)",
    )
    parser.add_argument(
        "--reviewer-case",
        action="store_true",
        help="run only the l-bench manifest reviewer_case",
    )
    parser.add_argument(
        "--canonicalization",
        choices=["equality", "max-lower", "intersection"],
        default="equality",
        help="l-bench state-class canonicalization mode (default: equality)",
    )
    parser.add_argument(
        "--extrapolation",
        action="store_true",
        help="enable l-bench k-extrapolation of clock zones",
    )
    parser.add_argument(
        "--validate-tdg-only",
        action="store_true",
        help="l-bench: export TDG DOT only; skip PTPN lowering and SCG analysis",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print planned runs without executing",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.skip_suites and args.skip_lbench:
        print(
            "error: both --skip-suites and --skip-lbench given; nothing to do",
            file=sys.stderr,
        )
        return 1

    ptpn_bin = resolve_ptpn_bin(args.ptpn)

    if not args.dry_run:
        ensure_ptpn(ptpn_bin)

    failures = 0
    if not args.skip_suites:
        failures += run_suites(ptpn_bin, args)
    if not args.skip_lbench:
        failures += run_lbench(ptpn_bin, args)

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
