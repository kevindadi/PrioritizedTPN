#!/usr/bin/env python3
"""Regenerate webapp/demo/demo-data.js from the bundled demo examples.

The generator imports the analysis pipeline from ``server.py`` (it does not
start the HTTP server), runs every example marked ``demo`` with its recommended
options, and writes a single JavaScript file:

    window.PTPN_DEMO_DATA = {
      "generated_at": "...",
      "status": {...},
      "examples": [{... example fields ..., "source": "...", "response": {...}}]
    };

The file is intended for the static/artifact demo, so it must stay below
~8 MB.  ``</`` sequences are escaped so the payload can be embedded in HTML.
"""

from __future__ import annotations

import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

import server


MAX_DEMO_BYTES = 8 * 1024 * 1024
_TEMP_PATH_RE = re.compile(r"/tmp/ptpn-web-[^/\s\"']+/")


def _sanitize_text(text: str) -> str:
    return _TEMP_PATH_RE.sub("", text)


def _utc_timestamp() -> str:
    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="seconds")
        .replace("+00:00", "Z")
    )


def _js_payload(payload) -> str:
    text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    # Never emit a literal "</script>" inside the generated document.
    text = text.replace("</", "<\\/")
    # U+2028/U+2029 are valid JSON but were historically invalid in JS strings.
    text = text.replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    return "window.PTPN_DEMO_DATA = " + text + ";\n"


STORY_POLICIES = [
    ("fixed", ""),
    ("rm", "rm"),
    ("dm", "dm"),
    ("restart", "fixed_prior_with_restart"),
    ("edf", "edf"),
    ("fifo", "fifo"),
]

STORY_REPAIRS = {
    "multicore": """// LLM Revision Option A: Dual-core isolation + feasible deadline for task a
system deadline_miss_fixed_multicore {
  cpus 2
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task a: periodic {
    period 10ms
    deadline 6ms
    core 0
    priority 101
    time [3, 5] estimated by "profiler"
  }

  task b: periodic {
    period 2ms
    deadline 2ms
    core 1
    priority 100
    time [1, 1] estimated by "profiler"
  }

  start a
  start b
}
""",
    "priority": """// LLM Revision Option B: Rate-Monotonic priority assignment on single core
system deadline_miss_fixed_priority {
  cpus 1
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task a: periodic {
    period 10ms
    deadline 10ms
    core 0
    priority 100
    time [3, 5] estimated by "profiler"
  }

  task b: periodic {
    period 2ms
    deadline 2ms
    core 0
    priority 102
    time [1, 1] estimated by "profiler"
  }

  start a
  start b
}
""",
}


def _policy_summary(response) -> dict:
    """Compact analyzer result used by the story's policy comparison."""
    metrics = response.get("metrics") or {}
    analysis = response.get("analysis") or {}
    verdicts = {
        task.get("task"): task for task in analysis.get("tasks") or []
    }
    tasks = []
    for task in metrics.get("tasks") or []:
        name = task.get("name")
        verdict = verdicts.get(name) or {}
        tasks.append(
            {
                "name": name,
                "priority": task.get("priority"),
                "wcet": task.get("wcet"),
                "wcrt": task.get("wcrt"),
                "verdict": verdict.get("verdict") or task.get("verdict"),
            }
        )
    return {
        "exit_code": response.get("exit_code"),
        "ok": response.get("ok"),
        "states": metrics.get("states"),
        "exact": metrics.get("exact"),
        "schedulability": analysis.get("schedulability"),
        "tasks": tasks,
        "diagnostics": [
            {
                "code": diag.get("code"),
                "severity": diag.get("severity"),
                "subject": diag.get("subject"),
                "message": diag.get("message"),
            }
            for diag in response.get("diagnostics") or []
        ],
    }


def _summary_line(index: int, total: int, example, response) -> str:
    metrics = response.get("metrics")
    analysis = response.get("analysis") or {}
    states = analysis.get("states")
    if states is None and isinstance(metrics, dict):
        states = metrics.get("states")
    graphs = response.get("graphs") or {}
    scg = graphs.get("scg") or {}
    ptpn = graphs.get("ptpn") or {}
    scg_nodes = scg.get("node_count", 0)
    scg_edges = scg.get("edge_count", 0)
    ptpn_nodes = ptpn.get("node_count", 0)
    ptpn_edges = ptpn.get("edge_count", 0)
    if scg.get("truncated"):
        scg_nodes_text = "{}+truncated".format(scg_nodes)
    else:
        scg_nodes_text = str(scg_nodes)
    return (
        "[{}/{}] {:<14} exit={:<3} states={:<6} metrics={:<3} "
        "scg={}/{} ptpn={}/{}".format(
            index,
            total,
            example["id"],
            response.get("exit_code"),
            "-" if states is None else str(states),
            "yes" if isinstance(metrics, dict) else "no",
            scg_nodes_text,
            scg_edges,
            ptpn_nodes,
            ptpn_edges,
        )
    )


def generate(output_path: Path = None) -> int:
    if output_path is None:
        output_path = server.DEMO_DIR / "demo-data.js"

    examples = [example for example in server.list_examples() if example["demo"]]
    if not examples:
        print("error: no demo examples were found", file=sys.stderr)
        return 1

    status = server.get_status(demo=True)
    print("Generating demo data for {} examples...".format(len(examples)))
    print(
        "  binary:   {}{}".format(
            status["binary"],
            "" if status["binary_exists"] else "  [MISSING]",
        )
    )
    print("  graphviz: {}".format(status["graphviz"] or "not found"))
    if not status["binary_exists"]:
        print(
            "error: the ptpn binary is missing; build the project or set "
            "PTPN_BIN",
            file=sys.stderr,
        )
        return 1

    records = []
    for index, example in enumerate(examples, start=1):
        mode = server.DEMO_MODES.get(example["id"], "analyze")
        response = server.run_analysis(
            example["source"],
            example["name"],
            example["kind"],
            mode,
            example["options"],
        )
        response["logs"] = [_sanitize_text(line) for line in response.get("logs", [])]
        response["notices"] = [
            _sanitize_text(notice) for notice in response.get("notices", [])
        ]
        record = dict(example)
        record["response"] = response
        records.append(record)

        summary = _summary_line(index, len(examples), example, response)
        encoded_size = len(
            json.dumps(record, ensure_ascii=False, separators=(",", ":")).encode(
                "utf-8"
            )
        )
        print(summary + " bytes=" + _human_bytes(encoded_size))
        if response.get("notices"):
            for notice in response["notices"]:
                print("         notice: " + notice)

    demo_status = {
        "ok": True,
        "demo": True,
        "binary": "precomputed",
        "binary_exists": False,
        "version": status.get("version") or "precomputed",
        "graphviz": status.get("graphviz"),
        "repo_root": "",
        "build_dir": "",
    }

    # The story compares the same hw model under several firing rules.  Keep
    # only the compact verdict fields here; the full responses stay in
    # ``examples`` above.
    story = {"policies": {}, "repairs": {}}
    hw = next((example for example in examples if example["id"] == "hw"), None)
    if hw is not None:
        for label, policy in STORY_POLICIES:
            options = dict(hw["options"])
            options["policy"] = policy
            response = server.run_analysis(
                hw["source"], hw["name"], hw["kind"], "analyze", options
            )
            story["policies"][label] = _policy_summary(response)
            print(
                "[story] policy={:<8} exit={:<3} states={}".format(
                    label,
                    response.get("exit_code"),
                    story["policies"][label].get("states"),
                )
            )
    for repair_id, repair_source in STORY_REPAIRS.items():
        response = server.run_analysis(
            repair_source, "repaired.rtsl", "rtsl", "analyze", {}
        )
        story["repairs"][repair_id] = {
            "source": repair_source,
            "summary": _policy_summary(response),
        }
        print(
            "[story] repair={:<10} exit={:<3} sched={}".format(
                repair_id,
                response.get("exit_code"),
                story["repairs"][repair_id]["summary"].get("schedulability"),
            )
        )

    payload = {
        "generated_at": _utc_timestamp(),
        "status": demo_status,
        "examples": records,
        "story": story,
    }
    js_text = _js_payload(payload)
    js_bytes = js_text.encode("utf-8")
    if len(js_bytes) > MAX_DEMO_BYTES:
        print(
            "error: generated demo data is {} (limit is {}); reduce "
            "render_limit or demo examples".format(
                _human_bytes(len(js_bytes)), _human_bytes(MAX_DEMO_BYTES)
            ),
            file=sys.stderr,
        )
        return 1

    output_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        output_path.write_text(js_text, encoding="utf-8")
    except OSError as exc:
        print(
            "error: could not write {}: {}".format(output_path, exc),
            file=sys.stderr,
        )
        return 1

    print("")
    print(
        "Wrote {} ({}), {} examples".format(
            output_path, _human_bytes(len(js_bytes)), len(records)
        )
    )
    print("Generated at {}".format(payload["generated_at"]))
    return 0


def _human_bytes(count: int) -> str:
    value = float(count)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024.0 or unit == "GB":
            if unit == "B":
                return "{:.0f} B".format(value)
            return "{:.1f} {}".format(value, unit)
        value /= 1024.0
    return "{:.1f} GB".format(value)


def main(argv=None) -> int:
    output_path = server.DEMO_DIR / "demo-data.js"
    if argv:
        if len(argv) > 1:
            print(
                "usage: python3 webapp/make_demo.py [OUTPUT_PATH]",
                file=sys.stderr,
            )
            return 2
        output_path = Path(argv[0]).expanduser().resolve()
    return generate(output_path)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
