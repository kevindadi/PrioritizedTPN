#!/usr/bin/env python3
"""Local web backend for the PTPN analyzer.

This is a zero-dependency (Python 3.9 standard library only) HTTP server that
wraps the ``ptpn`` command-line binary and returns structured JSON for the
browser UI.  The analysis pipeline itself is exposed as a plain function
(:func:`run_analysis`) so ``make_demo.py`` can precompute demo responses
without starting a server.

Design notes
------------
* The client never supplies a file path: the submitted source is written to a
  fresh temporary directory and handed to ``ptpn`` as ``model.rtsl`` or
  ``model.json``.
* All exports (TDG/PTPN/SCG/metrics/WCET) are temporary files in that directory
  and are deleted when the request finishes.
* Graphviz DOT exports are parsed locally and, when the node count is within
  the render limit, laid out with ``dot -Tjson``.  The result is a
  browser-friendly node/edge graph with coordinates in points and a top-left
  origin.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

# ---------------------------------------------------------------------------
# Paths and constants
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
WEBAPP_DIR = REPO_ROOT / "webapp"
STATIC_DIR = WEBAPP_DIR / "static"
DEMO_DIR = WEBAPP_DIR / "demo"

MAX_SOURCE_BYTES = 2 * 1024 * 1024          # /api/analyze source limit
MAX_BODY_BYTES = 4 * 1024 * 1024            # HTTP request body limit
ANALYSIS_TIMEOUT_SECONDS = 180
DOT_TIMEOUT_SECONDS = 60
DEFAULT_RENDER_LIMIT = 1800
LOG_LINE_LIMIT = 300

DEFAULT_OPTIONS = {
    "max_states": 10000,
    "canonicalization": "equality",
    "reduction": "none",
    "policy": "",
    "horizon": None,
    "extrapolation": False,
    "keep_suspension_clocks": False,
    "render_limit": DEFAULT_RENDER_LIMIT,
}

OPTION_KEYS = set(DEFAULT_OPTIONS)
CANONICALIZATION_VALUES = {"equality", "max-lower", "intersection"}
REDUCTION_VALUES = {"none", "upper-bound", "cone", "symmetry"}
KIND_VALUES = {"rtsl", "tdg"}
MODE_VALUES = {"check", "analyze"}

# Demo examples that need a mode other than "analyze".
DEMO_MODES = {
    "pending-demo": "check",
    "syntax-error": "check",
}

ANSI_RE = re.compile(
    r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))"
)

_LOG_LOCK = threading.Lock()


def _log_line(text: str) -> None:
    with _LOG_LOCK:
        print(text, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# Example catalogue
# ---------------------------------------------------------------------------


def _base_options(**overrides) -> dict:
    options = dict(DEFAULT_OPTIONS)
    options.pop("render_limit", None)  # examples use the server default
    options.update(overrides)
    return options


EXAMPLE_SPECS = [
    {
        "id": "intro-demo",
        "path": "example/introduction/demo-filled.rtsl",
        "title": "传感器融合流水线",
        "kind": "rtsl",
        "demo": True,
        "description": (
            "三核固定优先级流水线：sensor 经 fork 分发到带互斥锁的 fusion 与 "
            "logger，再 join 到 display；所有 WCET 区间已填写。"
        ),
        "tags": ["RTSL", "fork/join", "互斥锁", "周期任务"],
        "options": _base_options(),
    },
    {
        "id": "pending-demo",
        "path": "example/introduction/demo.rtsl",
        "title": "待填写测量",
        "kind": "rtsl",
        "demo": True,
        "description": (
            "与传感器融合示例相同，但 fusion 的时间仍为 measured by llm；"
            "ptpn check 会在 pending_measurements 中列出待填写的任务。"
        ),
        "tags": ["RTSL", "待填写", "check", "measured by llm"],
        "options": _base_options(),
    },
    {
        "id": "locked",
        "path": "example/introduction/locked.rtsl",
        "title": "互斥锁分段",
        "kind": "rtsl",
        "demo": False,
        "description": (
            "单核最小互斥锁示例：一个任务按 2*锁数+1 = 3 段执行区间建模临界区。"
        ),
        "tags": ["RTSL", "互斥锁", "执行分段", "最小示例"],
        "options": _base_options(),
    },
    {
        "id": "core-any",
        "path": "example/introduction/core-any.rtsl",
        "title": "Core Any 分配域",
        "kind": "rtsl",
        "demo": False,
        "description": (
            "core any 分配域示例：check 通过并给出提示，rtsl 分析以 "
            "E-VAL-218 拒绝，需改用 allocate。"
        ),
        "tags": ["RTSL", "core any", "allocate"],
        "options": _base_options(),
    },
    {
        "id": "syntax-error",
        "path": "example/diagnostics/syntax-error.rtsl",
        "title": "语法错误",
        "kind": "rtsl",
        "demo": True,
        "description": "任务块缺少右花括号，触发 E-SYN-102 语法错误（退出码 2）。",
        "tags": ["诊断", "语法错误", "E-SYN-102"],
        "options": _base_options(),
    },
    {
        "id": "validation-error",
        "path": "example/diagnostics/validation-error.rtsl",
        "title": "校验错误",
        "kind": "rtsl",
        "demo": False,
        "description": "周期任务的 period 为 0，触发 E-VAL-206 校验错误（退出码 3）。",
        "tags": ["诊断", "校验错误", "E-VAL-206"],
        "options": _base_options(),
    },
    {
        "id": "unsupported",
        "path": "example/diagnostics/unsupported-edf-locks.rtsl",
        "title": "不支持类",
        "kind": "rtsl",
        "demo": False,
        "description": (
            "EDF 策略下任务使用互斥锁，超出支持的静态划分类，lowering 触发 "
            "E-UNS-001（退出码 4）。"
        ),
        "tags": ["诊断", "EDF", "不支持类", "E-UNS-001"],
        "options": _base_options(),
    },
    {
        "id": "spin-lock",
        "path": "example/diagnostics/spin-lock.rtsl",
        "title": "自旋锁",
        "kind": "rtsl",
        "demo": False,
        "description": (
            "单自旋锁最小复现：当前 lowering 把第 1 段标记为不可挂起，"
            "临界区语义与预期不同（已知限制）。"
        ),
        "tags": ["诊断", "自旋锁", "已知限制"],
        "options": _base_options(),
    },
    {
        "id": "hw",
        "path": "example/motivating-examples/hw.json",
        "title": "优先级逆转",
        "kind": "tdg",
        "demo": True,
        "description": (
            "单核固定优先级动机示例：周期任务 A 依赖 B，低优先级任务 F 与 A "
            "同时就绪，展示优先级逆转/阻塞。"
        ),
        "tags": ["TDG", "固定优先级", "优先级逆转", "周期任务"],
        "options": _base_options(),
    },
    {
        "id": "deadlock",
        "path": "example/p-bench/single-core-diff-priority.json",
        "title": "优先级竞争 / 死锁",
        "kind": "tdg",
        "demo": True,
        "description": (
            "单核固定优先级竞争：A/B/C 与 D/E/F 两条链在 C 汇合，"
            "产生 18 个死锁状态并以退出码 6 结束。"
        ),
        "tags": ["TDG", "死锁", "单核", "退出码 6"],
        "options": _base_options(max_states=500, render_limit=500),
    },
    {
        "id": "s-bench-a",
        "path": "example/s-bench/a.json",
        "title": "S-Bench A",
        "kind": "tdg",
        "demo": False,
        "description": "双核 S-Bench A：周期任务 A、D 各自展开依赖链，并在 C 汇合。",
        "tags": ["TDG", "基准", "双核", "周期任务"],
        "options": _base_options(),
    },
    {
        "id": "symmetry-4",
        "path": "example/symmetry/symmetric-4t-4c.json",
        "title": "4 任务对称",
        "kind": "tdg",
        "demo": True,
        "description": (
            "4 个同构任务分别绑定 4 个核，启用对称性归约；指标为非精确"
            "（exact=false）。"
        ),
        "tags": ["TDG", "对称性", "归约", "exact=false"],
        "options": _base_options(reduction="symmetry"),
    },
    {
        "id": "pipeline-20",
        "path": "example/l-bench/pipeline-20t-4c.json",
        "title": "20 任务流水线",
        "kind": "tdg",
        "demo": True,
        "description": (
            "20 任务双通道流水线，4 核与 1 个共享互斥锁；默认使用 20000 "
            "状态上限完成分析。"
        ),
        "tags": ["TDG", "规模", "流水线", "互斥锁"],
        "options": _base_options(max_states=20000),
    },
]


def list_examples() -> list:
    """Return every bundled example that exists on disk, with its source."""
    examples = []
    for spec in EXAMPLE_SPECS:
        source_path = REPO_ROOT / spec["path"]
        if not source_path.is_file():
            continue
        try:
            source = source_path.read_text(encoding="utf-8")
        except OSError:
            continue
        examples.append(
            {
                "id": spec["id"],
                "name": source_path.name,
                "title": spec["title"],
                "kind": spec["kind"],
                "path": spec["path"],
                "source": source,
                "description": spec["description"],
                "tags": list(spec["tags"]),
                "demo": bool(spec["demo"]),
                "options": dict(spec["options"]),
            }
        )
    return examples


# ---------------------------------------------------------------------------
# Binary discovery / environment status
# ---------------------------------------------------------------------------


class BinaryMissingError(RuntimeError):
    """Raised when the configured ptpn binary cannot be executed."""


def is_executable(path: Path) -> bool:
    try:
        return path.is_file() and os.access(str(path), os.X_OK)
    except OSError:
        return False


def _build_dir_label(binary: Path) -> str:
    parent = binary.parent.resolve()
    try:
        return str(parent.relative_to(REPO_ROOT))
    except ValueError:
        return str(parent)


def discover_binary() -> tuple:
    """Resolve the ptpn binary.

    Order: ``$PTPN_BIN`` -> ``<repo>/.ptpn-build-dir``/ptpn -> ``build/ptpn``.
    Returns ``(path, build_dir_label)``.  The path is returned even when the
    file is missing so callers can report it clearly.
    """
    env_value = os.environ.get("PTPN_BIN", "").strip()
    if env_value:
        candidate = Path(env_value).expanduser()
        if not candidate.is_absolute():
            candidate = REPO_ROOT / candidate
        candidate = candidate.resolve()
        return candidate, _build_dir_label(candidate)

    marker = REPO_ROOT / ".ptpn-build-dir"
    if marker.is_file():
        try:
            marker_value = marker.read_text(encoding="utf-8").strip()
        except OSError:
            marker_value = ""
        if marker_value:
            build_base = Path(marker_value).expanduser()
            if not build_base.is_absolute():
                build_base = REPO_ROOT / build_base
            candidate = (build_base / "ptpn").resolve()
            if is_executable(candidate):
                return candidate, _build_dir_label(candidate)

    candidate = (REPO_ROOT / "build" / "ptpn").resolve()
    return candidate, _build_dir_label(candidate)


def _dot_binary():
    env_value = os.environ.get("DOT_BIN", "").strip()
    if env_value:
        return env_value
    found = shutil.which("dot")
    if found:
        return found
    fallback = Path("/usr/local/bin/dot")
    if is_executable(fallback):
        return str(fallback)
    return None


def get_graphviz_version():
    dot = _dot_binary()
    if not dot:
        return None
    try:
        proc = subprocess.run(
            [dot, "-V"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    output = _decode_bytes(proc.stdout) + _decode_bytes(proc.stderr)
    match = re.search(r"graphviz version ([0-9][^\s(]*)", output)
    return match.group(1) if match else None


def get_status(demo: bool = False) -> dict:
    binary, build_dir = discover_binary()
    binary_exists = is_executable(binary)
    version = None
    binary_error = None
    if binary_exists:
        try:
            proc = subprocess.run(
                [str(binary), "--version"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=15,
            )
            output = _decode_bytes(proc.stdout).strip()
            if output:
                version = output.splitlines()[0].strip()
            elif proc.returncode != 0:
                binary_error = "ptpn --version exited with code {}".format(
                    proc.returncode
                )
        except (OSError, subprocess.TimeoutExpired) as exc:
            binary_error = str(exc)
    else:
        binary_error = "ptpn binary not found or not executable: {}".format(binary)

    status = {
        "ok": True,
        "demo": bool(demo),
        "binary": str(binary),
        "binary_exists": binary_exists,
        "version": version,
        "graphviz": get_graphviz_version(),
        "repo_root": str(REPO_ROOT),
        "build_dir": build_dir,
    }
    if binary_error:
        status["binary_error"] = binary_error
    return status


# ---------------------------------------------------------------------------
# Option validation
# ---------------------------------------------------------------------------


def _require_int(value, name: str, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError("{} must be an integer".format(name))
    if value < low or value > high:
        raise ValueError("{} must be between {} and {}".format(name, low, high))
    return value


def _require_bool(value, name: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError("{} must be a boolean".format(name))
    return value


def normalize_options(raw) -> dict:
    """Validate the client option object and fill in defaults."""
    if raw is None:
        raw = {}
    if not isinstance(raw, dict):
        raise ValueError("options must be a JSON object")
    unknown = sorted(set(raw) - OPTION_KEYS)
    if unknown:
        raise ValueError("unknown option(s): {}".format(", ".join(unknown)))

    options = dict(DEFAULT_OPTIONS)
    if "max_states" in raw:
        options["max_states"] = _require_int(raw["max_states"], "max_states", 1, 200000)
    if "canonicalization" in raw:
        value = raw["canonicalization"]
        if not isinstance(value, str) or value not in CANONICALIZATION_VALUES:
            raise ValueError(
                "canonicalization must be one of: {}".format(
                    ", ".join(sorted(CANONICALIZATION_VALUES))
                )
            )
        options["canonicalization"] = value
    if "reduction" in raw:
        value = raw["reduction"]
        if not isinstance(value, str) or value not in REDUCTION_VALUES:
            raise ValueError(
                "reduction must be one of: {}".format(
                    ", ".join(sorted(REDUCTION_VALUES))
                )
            )
        options["reduction"] = value
    if "policy" in raw:
        value = raw["policy"]
        if value is None:
            value = ""
        if not isinstance(value, str):
            raise ValueError("policy must be a string")
        value = value.strip()
        if len(value) > 64:
            raise ValueError("policy is too long (maximum 64 characters)")
        if value and not re.fullmatch(r"[A-Za-z0-9_.-]+", value):
            raise ValueError(
                "policy may only contain letters, digits, '_', '.' and '-'"
            )
        options["policy"] = value
    if "horizon" in raw:
        value = raw["horizon"]
        if value is not None:
            value = _require_int(value, "horizon", 0, 2147483647)
        options["horizon"] = value
    if "extrapolation" in raw:
        options["extrapolation"] = _require_bool(
            raw["extrapolation"], "extrapolation"
        )
    if "keep_suspension_clocks" in raw:
        options["keep_suspension_clocks"] = _require_bool(
            raw["keep_suspension_clocks"], "keep_suspension_clocks"
        )
    if "render_limit" in raw:
        options["render_limit"] = _require_int(
            raw["render_limit"], "render_limit", 1, 100000
        )
    return options


def _sanitize_filename(filename, kind: str) -> str:
    default = "model.rtsl" if kind == "rtsl" else "model.json"
    if filename is None:
        return default
    if not isinstance(filename, str):
        raise ValueError("filename must be a string")
    filename = filename.strip()
    if not filename:
        return default
    if (
        len(filename) > 128
        or "\x00" in filename
        or "/" in filename
        or "\\" in filename
        or filename in (".", "..")
    ):
        raise ValueError("filename must be a plain file name, not a path")
    return filename


# ---------------------------------------------------------------------------
# DOT parsing and graph conversion
# ---------------------------------------------------------------------------

NODE_RE = re.compile(r'^\s*"?([A-Za-z0-9_]+)"?\s+\[(.*)\];\s*$')
EDGE_RE = re.compile(
    r'^\s*"?([A-Za-z0-9_]+)"?\s*->\s*"?([A-Za-z0-9_]+)"?\s*\[(.*)\];\s*$'
)
ATTR_KEY_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*")
GRAPH_STATEMENT_NAMES = {"graph", "node", "edge", "digraph", "subgraph"}


def _decode_quoted_dot_string(raw: str) -> str:
    """Decode the contents of a quoted DOT attribute value.

    The PTPN writers emit newlines as ``\\\\n`` (a double-escaped ``\\n``), so
    both single- and double-escaped newline forms are normalized to a real
    newline here.
    """
    out = []
    i = 0
    length = len(raw)
    while i < length:
        char = raw[i]
        if char != "\\" or i + 1 >= length:
            out.append(char)
            i += 1
            continue
        nxt = raw[i + 1]
        if nxt == "\\":
            # ``\\n`` -> newline, ``\\`` -> literal backslash.
            if i + 2 < length and raw[i + 2] == "n":
                out.append("\n")
                i += 3
            else:
                out.append("\\")
                i += 2
            continue
        if nxt == "n":
            out.append("\n")
        elif nxt == "t":
            out.append("\t")
        elif nxt == '"':
            out.append('"')
        else:
            out.append(nxt)
        i += 2
    return "".join(out)


def _parse_attributes(text: str) -> dict:
    """Parse ``key=value`` attribute lists, including HTML ``<...>`` labels."""
    attrs = {}
    i = 0
    length = len(text)
    while i < length:
        while i < length and text[i] in " \t,;":
            i += 1
        if i >= length:
            break
        match = ATTR_KEY_RE.match(text, i)
        if not match:
            comma = text.find(",", i)
            if comma == -1:
                break
            i = comma + 1
            continue
        key = match.group(1)
        i = match.end()
        if i < length and text[i] == '"':
            i += 1
            chunk = []
            while i < length:
                if text[i] == "\\" and i + 1 < length:
                    chunk.append(text[i : i + 2])
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                chunk.append(text[i])
                i += 1
            attrs[key] = _decode_quoted_dot_string("".join(chunk))
        elif i < length and text[i] == "<":
            start = i
            i += 1
            while i < length:
                if text[i] == ">":
                    lookahead = i + 1
                    while lookahead < length and text[lookahead] in " \t":
                        lookahead += 1
                    if lookahead >= length or text[lookahead] == ",":
                        break
                i += 1
            end = min(i + 1, length)
            attrs[key] = text[start:end]
            i = end
        else:
            start = i
            while i < length and text[i] != ",":
                i += 1
            attrs[key] = text[start:i].strip()
    return attrs


def parse_dot(text: str):
    """Parse the predictable one-statement-per-line DOT written by PTPN."""
    nodes = []
    edges = []
    for line in text.splitlines():
        edge_match = EDGE_RE.match(line)
        if edge_match:
            edges.append(
                {
                    "from": edge_match.group(1),
                    "to": edge_match.group(2),
                    "attrs": _parse_attributes(edge_match.group(3)),
                }
            )
            continue
        node_match = NODE_RE.match(line)
        if node_match:
            name = node_match.group(1)
            if name.lower() in GRAPH_STATEMENT_NAMES:
                continue
            nodes.append(
                {
                    "id": name,
                    "attrs": _parse_attributes(node_match.group(2)),
                }
            )
    return nodes, edges


def _to_float(value, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _round(value: float) -> float:
    return round(value, 2)


def _parse_point(value):
    if not isinstance(value, str):
        return None
    value = value.strip().rstrip("!")
    parts = value.split(",")
    if len(parts) != 2:
        return None
    try:
        return float(parts[0]), float(parts[1])
    except ValueError:
        return None


def _graphviz_binary(engine: str = "dot"):
    """Resolve a Graphviz layout engine (dot/neato/sfdp/...)."""
    engine = (engine or "dot").strip() or "dot"
    if engine == "dot":
        return _dot_binary()
    env_value = os.environ.get("DOT_BIN", "").strip()
    if env_value:
        sibling = Path(env_value).with_name(engine)
        if is_executable(sibling):
            return str(sibling)
    found = shutil.which(engine)
    if found:
        return found
    fallback = Path("/usr/local/bin") / engine
    if is_executable(fallback):
        return str(fallback)
    return _dot_binary()


def _run_dot_layout(dot_path: Path, engine: str = "dot", extra_args=()):
    """Run Graphviz ``<engine> -Tjson``; return ``(data, error_message)``."""
    binary = _graphviz_binary(engine)
    if not binary:
        return None, "Graphviz '{}' was not found; graph has no layout".format(engine)
    try:
        proc = subprocess.run(
            [binary, *extra_args, "-Tjson", str(dot_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=DOT_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        return None, "Graphviz '{}' timed out after {} s".format(
            engine, DOT_TIMEOUT_SECONDS
        )
    except OSError as exc:
        return None, "failed to run Graphviz '{}': {}".format(engine, exc)
    if proc.returncode != 0:
        message = _decode_bytes(proc.stderr).strip().splitlines()
        detail = message[-1] if message else "no error output"
        return None, "Graphviz '{}' failed (exit {}): {}".format(
            engine, proc.returncode, detail[:300]
        )
    try:
        data = json.loads(_decode_bytes(proc.stdout))
    except ValueError:
        return None, "Graphviz '{}' produced unparseable JSON".format(engine)
    if not isinstance(data, dict):
        return None, "Graphviz '{}' produced an unexpected JSON document".format(engine)
    return data, None


def _layout_geometry(data: dict):
    """Extract the bounding box and the gvid -> name map from dot JSON."""
    bb = data.get("bb")
    if not isinstance(bb, str):
        return None
    parts = bb.split(",")
    if len(parts) != 4:
        return None
    try:
        x0, y0, x1, y1 = (float(part) for part in parts)
    except ValueError:
        return None
    width = x1 - x0
    height = y1 - y0
    objects = data.get("objects")
    if not isinstance(objects, list):
        objects = []
    gvid_to_name = {}
    for index, obj in enumerate(objects):
        if not isinstance(obj, dict):
            continue
        gvid = obj.get("_gvid", index)
        name = obj.get("name")
        if not isinstance(name, str) or not name:
            name = None
        gvid_to_name[gvid] = name
    return {
        "width": width,
        "height": height,
        "objects": objects,
        "gvid_to_name": gvid_to_name,
    }


def _layout_node_positions(geometry: dict) -> dict:
    height = geometry["height"]
    positions = {}
    objects = geometry["objects"]
    for index, obj in enumerate(objects):
        if not isinstance(obj, dict):
            continue
        name = obj.get("name")
        if not isinstance(name, str) or not name:
            continue
        point = _parse_point(obj.get("pos"))
        if point is None:
            continue
        width = _to_float(obj.get("width")) * 72.0
        node_height = _to_float(obj.get("height")) * 72.0
        positions[name] = {
            "x": _round(point[0]),
            "y": _round(height - point[1]),
            "w": _round(width),
            "h": _round(node_height),
        }
    return positions


def _edge_points(pos, height: float) -> list:
    """Parse Graphviz spline points, dropping ``e,``/``s,`` markers."""
    if not isinstance(pos, str):
        return []
    points = []
    for token in pos.split():
        token = token.strip()
        if not token:
            continue
        if len(token) > 1 and token[0] in ("e", "s") and token[1] == ",":
            # The endpoint marker token is dropped per the UI contract.
            continue
        point = _parse_point(token)
        if point is not None:
            points.append([_round(point[0]), _round(height - point[1])])
    return points


def _layout_edges(data: dict, geometry: dict) -> dict:
    """Map (tail-name, head-name) -> list of point lists, in DOT order."""
    height = geometry["height"]
    gvid_to_name = geometry["gvid_to_name"]
    edges = data.get("edges")
    if not isinstance(edges, list):
        return {}
    by_pair = {}
    for edge in edges:
        if not isinstance(edge, dict):
            continue
        tail = edge.get("tail")
        head = edge.get("head")
        if isinstance(tail, int) and not isinstance(tail, bool):
            tail_name = gvid_to_name.get(tail)
        elif isinstance(tail, str):
            tail_name = gvid_to_name.get(tail, tail)
        else:
            tail_name = None
        if isinstance(head, int) and not isinstance(head, bool):
            head_name = gvid_to_name.get(head)
        elif isinstance(head, str):
            head_name = gvid_to_name.get(head, head)
        else:
            head_name = None
        if not tail_name or not head_name:
            continue
        by_pair.setdefault((tail_name, head_name), []).append(
            _edge_points(edge.get("pos"), height)
        )
    return by_pair


def _classify_ptpn_transition(node_id: str) -> str:
    if "exec" in node_id:
        return "exec"
    if "get_core" in node_id:
        return "acquire"
    if "lock" in node_id or "unlock" in node_id:
        return "lock"
    if node_id.startswith("Fork"):
        return "fork"
    if node_id.startswith("Join"):
        return "join"
    if (
        node_id.endswith("_fire")
        or node_id.endswith("_consume")
        or node_id.endswith("_release")
    ):
        return "control"
    return "transition"


def _enrich_ptpn_node(node: dict, attrs: dict, shared_locks) -> None:
    label = node.get("raw_label") or ""
    first_line = label.splitlines()[0].strip() if label else node["id"]
    node["text"] = first_line
    shape = (attrs.get("shape") or "").lower()

    if shape == "circle":
        node["kind"] = "place"
        marking = re.search(r"M=(\d+)", label)
        capacity = re.search(r"C=(\d+)", label)
        node["marking"] = int(marking.group(1)) if marking else None
        node["capacity"] = int(capacity.group(1)) if capacity else None
        node_id = node["id"]
        if "period" in node_id:
            node["kind"] = "period"
        elif node_id in shared_locks:
            node["kind"] = "resource"
        elif (
            node_id.endswith("entry")
            or node_id.endswith("ready")
            or node_id.endswith("exit")
        ):
            node["kind"] = "place"
        elif "hold" in node_id:
            node["kind"] = "hold"
        elif "seg_" in node_id:
            node["kind"] = "segment"
        else:
            node["kind"] = "place"
        return

    if shape == "box":
        node["kind"] = _classify_ptpn_transition(node["id"])
        core = re.search(r"core=(-?\d+)", label)
        priority = re.search(r"π=(-?\d+)", label)
        interval = re.search(r"I=\[\[(-?\d+),\s*(-?\d+)\]", label)
        node["core"] = int(core.group(1)) if core else None
        node["priority"] = int(priority.group(1)) if priority else None
        node["interval"] = (
            [int(interval.group(1)), int(interval.group(2))]
            if interval
            else None
        )
        return

    node["kind"] = "unknown"


def _parse_scg_detail(tooltip: str) -> dict:
    detail = {
        "elapsed": None,
        "marking": None,
        "enabled": None,
        "suspended": None,
        "zone": None,
    }
    if not tooltip:
        return detail
    lines = tooltip.splitlines()

    def value_after(prefix: str, line: str):
        value = line[len(prefix) :].strip()
        return value or None

    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("Elapsed time:"):
            raw = value_after("Elapsed time:", stripped)
            if raw is not None:
                try:
                    detail["elapsed"] = int(raw)
                except ValueError:
                    detail["elapsed"] = None
        elif stripped.startswith("Marking:"):
            detail["marking"] = value_after("Marking:", stripped)
        elif stripped.startswith("E_pri"):
            colon = stripped.find(":")
            if colon != -1:
                detail["enabled"] = stripped[colon + 1 :].strip() or None
        elif stripped.startswith("Suspended:"):
            detail["suspended"] = value_after("Suspended:", stripped)
        elif stripped == "Zone:":
            zone = "\n".join(lines[index + 1 :]).rstrip("\n")
            detail["zone"] = zone if zone.strip() else None
    return detail


def _enrich_scg_node(node: dict, attrs: dict, deadlock_states) -> None:
    node_id = node["id"]
    suffix = re.search(r"(\d+)$", node_id)
    node["label"] = "S" + suffix.group(1) if suffix else node_id
    node["raw_label"] = None
    node["detail"] = _parse_scg_detail(attrs.get("tooltip") or "")
    index = int(suffix.group(1)) if suffix else None
    is_deadlock = index is not None and index in deadlock_states
    node["kind"] = "deadlock" if is_deadlock else "state"
    node["deadlock"] = bool(is_deadlock)


def _scg_edge_fields(label: str) -> dict:
    fields = {
        "transition": None,
        "core": None,
        "dwell_min": None,
        "dwell_max": None,
        "at_min": None,
        "at_max": None,
    }
    if not label:
        return fields
    first_line = label.splitlines()[0]
    transition = re.match(r"^\s*([^(\s]+)\s*\(", first_line)
    if transition:
        fields["transition"] = transition.group(1)
    core = re.search(r"\bcore=(-?\d+)", label)
    if core:
        fields["core"] = int(core.group(1))
    dwell = re.search(r"dwell=\[\s*(-?\d+)\s*,\s*(-?\d+)\s*\]", label)
    if dwell:
        fields["dwell_min"] = int(dwell.group(1))
        fields["dwell_max"] = int(dwell.group(2))
    at = re.search(r"@\[\s*(-?\d+)\s*,\s*(-?\d+)\s*\]", label)
    if at:
        fields["at_min"] = int(at.group(1))
        fields["at_max"] = int(at.group(2))
    return fields


def _empty_graph(node_count: int, edge_count: int, truncated: bool) -> dict:
    if truncated:
        return {
            "nodes": [],
            "edges": [],
            "truncated": True,
            "node_count": node_count,
            "edge_count": edge_count,
            "layout": "none",
        }
    return {
        "nodes": [],
        "edges": [],
        "bb": None,
        "truncated": False,
        "node_count": node_count,
        "edge_count": edge_count,
        "layout": "none",
    }


def _build_graph_nodes(
    parsed_nodes,
    graph_type: str,
    positions,
    shared_locks,
    deadlock_states,
):
    result = []
    for parsed in parsed_nodes:
        attrs = parsed["attrs"]
        node = {
            "id": parsed["id"],
            "tooltip": attrs.get("tooltip") or None,
            "raw_label": attrs.get("label") if graph_type == "ptpn" else None,
        }
        if graph_type == "ptpn":
            _enrich_ptpn_node(node, attrs, shared_locks)
        else:
            _enrich_scg_node(node, attrs, deadlock_states)
        if positions is not None:
            position = positions.get(parsed["id"])
            if position:
                node.update(position)
        result.append(node)
    return result


def _build_graph_edges(
    parsed_edges,
    graph_type: str,
    layout_edges,
):
    result = []
    for parsed in parsed_edges:
        attrs = parsed["attrs"]
        label = attrs.get("label") or ""
        edge = {
            "from": parsed["from"],
            "to": parsed["to"],
            "label": label,
            "tooltip": attrs.get("tooltip") or None,
        }
        if graph_type == "scg":
            edge.update(_scg_edge_fields(label))
        if layout_edges is None:
            edge["points"] = []
        else:
            buckets = layout_edges.get((parsed["from"], parsed["to"]))
            edge["points"] = buckets.pop(0) if buckets else []
        result.append(edge)
    return result


def convert_dot_graph(
    dot_path: Path,
    graph_type: str,
    render_limit: int,
    shared_locks=(),
    deadlock_states=(),
):
    """Convert an exported PTPN/SCG DOT file into the UI graph schema.

    Returns ``(graph, notices)``.
    """
    notices = []
    try:
        text = dot_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return None, ["could not read {} DOT export: {}".format(graph_type, exc)]

    parsed_nodes, parsed_edges = parse_dot(text)
    node_count = len(parsed_nodes)
    edge_count = len(parsed_edges)
    label = "PTPN" if graph_type == "ptpn" else "SCG"

    if node_count > render_limit:
        notices.append(
            "{} graph omitted: {} nodes exceeds render_limit {} (layout skipped)".format(
                label, node_count, render_limit
            )
        )
        return _empty_graph(node_count, edge_count, truncated=True), notices

    # PTPN is a bipartite flow graph: sfdp keeps it compact enough to read at
    # fit. SCG is a cyclic reachability graph where neato's spring layout gives
    # a far better aspect ratio than dot's long left-to-right chains.
    if graph_type == "scg":
        engine = "neato"
        engine_args = ()
    else:
        engine = "dot"
        engine_args = ()
    data, error = _run_dot_layout(dot_path, engine, engine_args)
    geometry = _layout_geometry(data) if data is not None else None
    if geometry is None:
        if error is None:
            error = "Graphviz output did not contain a usable bounding box"
        notices.append("{} graph rendered without layout: {}".format(label, error))
        nodes = _build_graph_nodes(
            parsed_nodes, graph_type, None, shared_locks, deadlock_states
        )
        edges = _build_graph_edges(parsed_edges, graph_type, None)
        return _empty_graph(node_count, edge_count, truncated=False) | {
            "nodes": nodes,
            "edges": edges,
        }, notices

    positions = _layout_node_positions(geometry)
    layout_edges = _layout_edges(data, geometry)
    nodes = _build_graph_nodes(
        parsed_nodes, graph_type, positions, shared_locks, deadlock_states
    )
    edges = _build_graph_edges(parsed_edges, graph_type, layout_edges)
    graph = {
        "nodes": nodes,
        "edges": edges,
        "bb": {
            "w": _round(geometry["width"]),
            "h": _round(geometry["height"]),
        },
        "truncated": False,
        "node_count": node_count,
        "edge_count": edge_count,
        "layout": engine,
    }
    return graph, notices


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------


def _decode_bytes(data) -> str:
    if data is None:
        return ""
    if isinstance(data, str):
        return data
    return data.decode("utf-8", "replace")


def _strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def _read_json_file(path: Path):
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return None


def _add_notice(notices, message: str) -> None:
    if message and message not in notices:
        notices.append(message)


STATS_PATTERNS = {
    "lowering_ms": re.compile(r"\[STATS\]\s+TDG2PN lowering:\s*(\d+)\s*ms"),
    "scg_ms": re.compile(r"\[STATS\]\s+SCG build:\s*(\d+)\s*ms"),
    "metrics_ms": re.compile(r"\[STATS\]\s+Metrics analysis:\s*(\d+)\s*ms"),
    "total_ms": re.compile(
        r"\[STATS\]\s+(?:RTSL|TDG) pipeline \(total\):\s*(\d+)\s*ms"
    ),
}


def parse_timings(stderr_text: str) -> dict:
    timings = {
        "lowering_ms": None,
        "scg_ms": None,
        "metrics_ms": None,
        "total_ms": None,
    }
    for line in stderr_text.splitlines():
        for key, pattern in STATS_PATTERNS.items():
            match = pattern.search(line)
            if match:
                timings[key] = int(match.group(1))
    return timings


def _rewrite_location(location, temp_input: Path, client_filename: str) -> None:
    if not isinstance(location, dict):
        return
    file_value = location.get("file")
    if not isinstance(file_value, str) or not file_value:
        return
    candidate = Path(file_value)
    if (
        file_value == str(temp_input)
        or candidate.name in ("model.rtsl", "model.json")
        or candidate.resolve() == temp_input.resolve()
    ):
        location["file"] = client_filename


def _rewrite_pending_files(pending, temp_input: Path, client_filename: str) -> None:
    if not isinstance(pending, list):
        return
    for entry in pending:
        if not isinstance(entry, dict):
            continue
        inject = entry.get("inject")
        if isinstance(inject, dict):
            _rewrite_location(inject, temp_input, client_filename)


def _shared_lock_names(tdg) -> set:
    if not isinstance(tdg, dict):
        return set()
    configuration = tdg.get("configuration")
    if not isinstance(configuration, dict):
        return set()
    locks = configuration.get("shared_locks")
    if not isinstance(locks, list):
        return set()
    return {lock for lock in locks if isinstance(lock, str)}


def _deadlock_state_set(metrics) -> set:
    if not isinstance(metrics, dict):
        return set()
    raw = metrics.get("deadlock_states")
    if not isinstance(raw, list):
        return set()
    result = set()
    for value in raw:
        if isinstance(value, bool):
            continue
        try:
            result.add(int(value))
        except (TypeError, ValueError):
            continue
    return result


def _utf8_size(text: str) -> int:
    try:
        return len(text.encode("utf-8"))
    except UnicodeEncodeError:
        raise ValueError("source is not valid UTF-8 text")


def run_analysis(source, filename, kind, mode, options) -> dict:
    """Run ``ptpn`` on in-memory source and return the UI response object.

    Raises :class:`ValueError` for invalid input and
    :class:`BinaryMissingError` when the binary cannot be executed.
    """
    if not isinstance(source, str):
        raise ValueError("source must be a string")
    if _utf8_size(source) > MAX_SOURCE_BYTES:
        raise ValueError("source exceeds the 2 MB limit")
    if not isinstance(kind, str) or kind not in KIND_VALUES:
        raise ValueError("kind must be 'rtsl' or 'tdg'")
    if not isinstance(mode, str) or mode not in MODE_VALUES:
        raise ValueError("mode must be 'check' or 'analyze'")
    if mode == "check" and kind != "rtsl":
        raise ValueError("check mode only supports RTSL sources")
    filename = _sanitize_filename(filename, kind)
    opts = normalize_options(options)

    binary, _ = discover_binary()
    if not is_executable(binary):
        raise BinaryMissingError(
            "ptpn binary not found or not executable: {}. "
            "Build the project or set PTPN_BIN.".format(binary)
        )

    notices = []
    temp_dir = Path(tempfile.mkdtemp(prefix="ptpn-web-"))
    temp_input = temp_dir / ("model.rtsl" if kind == "rtsl" else "model.json")
    tdg_json_path = temp_dir / "tdg.json"
    ptpn_dot_path = temp_dir / "ptpn.dot"
    scg_dot_path = temp_dir / "scg.dot"
    metrics_path = temp_dir / "metrics.json"
    wcet_path = temp_dir / "wcet.json"

    try:
        temp_input.write_text(source, encoding="utf-8")

        if mode == "check":
            command = [
                str(binary),
                "check",
                "-f",
                str(temp_input),
                "--diagnostics",
                "json",
            ]
        else:
            command = [
                str(binary),
                "rtsl" if kind == "rtsl" else "tdg",
                "-f",
                str(temp_input),
                "--diagnostics",
                "json",
            ]
            if kind == "rtsl":
                command += ["--export-tdg", str(tdg_json_path)]
            command += [
                "--export-ptpn",
                str(ptpn_dot_path),
                "--export-scg",
                str(scg_dot_path),
                "--export-metrics",
                str(metrics_path),
                "--export-wcet",
                str(wcet_path),
                "-m",
                str(opts["max_states"]),
                "--canonicalization",
                opts["canonicalization"],
                "--reduction",
                opts["reduction"],
            ]
            if opts["policy"]:
                command += ["--policy", opts["policy"]]
            if opts["horizon"] is not None:
                command += ["--horizon", str(opts["horizon"])]
            if opts["extrapolation"]:
                command.append("--extrapolation")
            if opts["keep_suspension_clocks"]:
                command.append("--keep-suspension-clocks")

        started = time.monotonic()
        timed_out = False
        try:
            proc = subprocess.run(
                command,
                cwd=str(REPO_ROOT),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=ANALYSIS_TIMEOUT_SECONDS,
            )
            stdout_bytes = proc.stdout
            stderr_bytes = proc.stderr
            returncode = proc.returncode
        except subprocess.TimeoutExpired as exc:
            timed_out = True
            stdout_bytes = getattr(exc, "stdout", None) or b""
            stderr_bytes = getattr(exc, "stderr", None) or b""
            returncode = 124
            _add_notice(
                notices,
                "analysis timed out after {} s and was killed".format(
                    ANALYSIS_TIMEOUT_SECONDS
                ),
            )
        except OSError as exc:
            raise BinaryMissingError(
                "failed to execute {}: {}".format(binary, exc)
            )
        duration_ms = int(round((time.monotonic() - started) * 1000))

        stdout_text = _decode_bytes(stdout_bytes)
        stderr_text = _decode_bytes(stderr_bytes)

        envelope = None
        if stdout_text.strip():
            try:
                parsed = json.loads(stdout_text)
            except ValueError:
                parsed = None
            if isinstance(parsed, dict):
                envelope = parsed
            else:
                _add_notice(
                    notices,
                    "could not parse the diagnostics envelope from ptpn stdout",
                )

        if timed_out:
            exit_code = 124
        elif envelope is not None and isinstance(
            envelope.get("exit_code"), int
        ) and not isinstance(envelope.get("exit_code"), bool):
            exit_code = envelope["exit_code"]
        else:
            exit_code = returncode

        if envelope is None and not timed_out and returncode != 0:
            stripped_stderr = _strip_ansi(stderr_text).strip().splitlines()
            useful_lines = [
                line.strip()
                for line in stripped_stderr
                if line.strip()
                and not line.strip().startswith("Run with --help")
            ]
            first_stderr = useful_lines[0][:300] if useful_lines else ""
            message = "ptpn exited with code {} without a diagnostics envelope".format(
                returncode
            )
            if first_stderr:
                message += ": " + first_stderr
            _add_notice(notices, message)

        diagnostics = []
        if envelope is not None:
            raw_diagnostics = envelope.get("diagnostics")
            if isinstance(raw_diagnostics, list):
                diagnostics = raw_diagnostics
            for entry in diagnostics:
                if isinstance(entry, dict):
                    _rewrite_location(
                        entry.get("location"), temp_input, filename
                    )

        result = {}
        if envelope is not None and isinstance(envelope.get("result"), dict):
            result = envelope["result"]
        analysis = result.get("analysis")
        if not isinstance(analysis, dict):
            analysis = None
        pending = result.get("pending_measurements")
        if not isinstance(pending, list):
            pending = []
        _rewrite_pending_files(pending, temp_input, filename)

        metrics = _read_json_file(metrics_path)
        wcet = _read_json_file(wcet_path)
        if kind == "rtsl":
            tdg = _read_json_file(tdg_json_path)
            if not isinstance(tdg, dict):
                tdg = None
        else:
            try:
                parsed_tdg = json.loads(source)
            except ValueError:
                parsed_tdg = None
            tdg = parsed_tdg if isinstance(parsed_tdg, dict) else None

        shared_locks = _shared_lock_names(tdg)
        deadlock_states = _deadlock_state_set(metrics)

        graphs = {"ptpn": None, "scg": None}
        if mode == "analyze" and not timed_out:
            if ptpn_dot_path.is_file():
                graph, graph_notices = convert_dot_graph(
                    ptpn_dot_path,
                    "ptpn",
                    opts["render_limit"],
                    shared_locks=shared_locks,
                )
                graphs["ptpn"] = graph
                for notice in graph_notices:
                    _add_notice(notices, notice)
            elif exit_code in (0, 6):
                _add_notice(notices, "PTPN DOT export was not produced")

            if scg_dot_path.is_file():
                graph, graph_notices = convert_dot_graph(
                    scg_dot_path,
                    "scg",
                    opts["render_limit"],
                    deadlock_states=deadlock_states,
                )
                graphs["scg"] = graph
                for notice in graph_notices:
                    _add_notice(notices, notice)
            elif exit_code in (0, 6):
                _add_notice(notices, "SCG DOT export was not produced")

        logs = _strip_ansi(stderr_text).splitlines()[-LOG_LINE_LIMIT:]

        return {
            "ok": exit_code == 0,
            "exit_code": exit_code,
            "duration_ms": duration_ms,
            "diagnostics": diagnostics,
            "envelope": envelope,
            "analysis": analysis,
            "pending_measurements": pending,
            "metrics": metrics if isinstance(metrics, dict) else None,
            "wcet": wcet if isinstance(wcet, dict) else None,
            "tdg": tdg,
            "graphs": graphs,
            "timings": parse_timings(stderr_text),
            "logs": logs,
            "effective_options": dict(opts),
            "notices": notices,
        }
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------

MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".htm": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".mjs": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".webp": "image/webp",
    ".ico": "image/x-icon",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
    ".ttf": "font/ttf",
    ".map": "application/json; charset=utf-8",
    ".txt": "text/plain; charset=utf-8",
    ".wasm": "application/wasm",
}

WRAP_PREFIX = (
    b'<!doctype html><html><head><meta charset="utf-8">'
    b'<meta name="viewport" content="width=device-width, initial-scale=1, '
    b'viewport-fit=cover"></head><body>'
)
WRAP_SUFFIX = b"</body></html>"


def _mime_for(path: Path) -> str:
    return MIME_TYPES.get(path.suffix.lower(), "application/octet-stream")


def _safe_resolve(base: Path, relative: str):
    """Resolve ``relative`` under ``base``; return None on traversal/miss."""
    if not relative or "\x00" in relative or "\\" in relative:
        return None
    try:
        base_resolved = base.resolve()
        candidate = (base / relative).resolve()
        common = os.path.commonpath([str(base_resolved), str(candidate)])
    except (OSError, RuntimeError, ValueError):
        return None
    if common != str(base_resolved):
        return None
    if not candidate.is_file():
        return None
    return candidate


def _looks_like_full_html(data: bytes) -> bool:
    head = data[:1024].lower()
    return b"<!doctype" in head or b"<html" in head


class PTPNRequestHandler(BaseHTTPRequestHandler):
    server_version = "PTPNWeb/1.0"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    # -- logging ---------------------------------------------------------
    def log_message(self, fmt, *args):  # noqa: A003 - stdlib signature
        pass

    def log_request(self, code="-", size="-"):
        pass

    def send_response(self, code, message=None):
        self._status = code
        super().send_response(code, message)

    def _request_log(self, method: str, started: float) -> None:
        path = urlsplit(self.path).path
        status = getattr(self, "_status", 0) or 0
        elapsed = (time.monotonic() - started) * 1000.0
        _log_line(
            "[webapp] {} {} {} {:.0f} ms".format(method, path, status, elapsed)
        )

    # -- responses -------------------------------------------------------
    def _send_bytes(self, code: int, body: bytes, content_type: str, cache: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", cache)
        if self.close_connection:
            self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_json(self, code: int, payload) -> None:
        body = json.dumps(
            payload, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        self._send_bytes(code, body, "application/json; charset=utf-8", "no-store")

    def _send_text(self, code: int, text: str) -> None:
        body = text.encode("utf-8")
        self._send_bytes(code, body, "text/plain; charset=utf-8", "no-store")

    def _send_html(self, code: int, body: bytes) -> None:
        self._send_bytes(code, body, "text/html; charset=utf-8", "no-store")

    # -- routing ---------------------------------------------------------
    def do_GET(self):  # noqa: N802 - stdlib signature
        self._dispatch_get("GET")

    def do_HEAD(self):  # noqa: N802 - stdlib signature
        self._dispatch_get("HEAD")

    def _dispatch_get(self, method: str) -> None:
        started = time.monotonic()
        try:
            self._handle_get()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:  # defensive: never take the server down
            _log_line("[webapp] internal error on {}: {!r}".format(self.path, exc))
            try:
                self._send_json(
                    500, {"ok": False, "error": "internal server error"}
                )
            except Exception:
                pass
        finally:
            self._request_log(method, started)

    def do_POST(self):  # noqa: N802 - stdlib signature
        started = time.monotonic()
        try:
            self._handle_post()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:
            _log_line("[webapp] internal error on {}: {!r}".format(self.path, exc))
            try:
                self._send_json(
                    500, {"ok": False, "error": "internal server error"}
                )
            except Exception:
                pass
        finally:
            self._request_log("POST", started)

    def _demo_mode(self) -> bool:
        return bool(getattr(self.server, "demo_mode", False))

    def _handle_get(self) -> None:
        path = urlsplit(self.path).path
        demo = self._demo_mode()

        if path == "/api/status":
            self._send_json(200, get_status(demo=demo))
            return
        if path == "/api/examples":
            self._send_json(200, {"examples": list_examples()})
            return
        if path.startswith("/api/"):
            self._send_json(
                404, {"ok": False, "error": "unknown API endpoint: " + path}
            )
            return
        if path in ("/", "/index.html"):
            self._serve_index(demo)
            return
        if path.startswith("/static/"):
            relative = unquote(path[len("/static/") :])
            resolved = None
            if demo and relative == "demo-data.js":
                resolved = _safe_resolve(DEMO_DIR, relative)
            if resolved is None:
                resolved = _safe_resolve(STATIC_DIR, relative)
            self._serve_file(resolved)
            return
        if path.startswith("/demo/"):
            relative = unquote(path[len("/demo/") :])
            self._serve_file(_safe_resolve(DEMO_DIR, relative))
            return

        # The page references style.css / app.js / demo-data.js relative to
        # itself, so root-level assets fall back to the static (and, in demo
        # mode, demo) directories.
        relative = unquote(path.lstrip("/"))
        resolved = None
        if demo:
            resolved = _safe_resolve(DEMO_DIR, relative)
        if resolved is None:
            resolved = _safe_resolve(STATIC_DIR, relative)
        self._serve_file(resolved)

    def _serve_index(self, demo: bool) -> None:
        if demo:
            demo_index = DEMO_DIR / "index.html"
            if demo_index.is_file():
                try:
                    data = demo_index.read_bytes()
                except OSError:
                    data = None
                if data is not None:
                    if not _looks_like_full_html(data):
                        data = WRAP_PREFIX + data + WRAP_SUFFIX
                    self._send_html(200, data)
                    return

        static_index = STATIC_DIR / "index.html"
        if static_index.is_file():
            try:
                data = static_index.read_bytes()
            except OSError as exc:
                self._send_text(500, "could not read index: {}\n".format(exc))
                return
            # The frontend index is authored as a fragment; wrap it here.
            data = WRAP_PREFIX + data + WRAP_SUFFIX
            self._send_html(200, data)
            return

        message = (
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>PTPN web UI</title></head><body>"
            "<h1>PTPN web UI</h1>"
            "<p><code>webapp/static/index.html</code> is missing.</p>"
            "<p>The JSON API is available at "
            "<code>/api/status</code>, <code>/api/examples</code> and "
            "<code>/api/analyze</code>.</p>"
            "</body></html>"
        ).encode("utf-8")
        self._send_html(404, message)

    def _serve_file(self, path) -> None:
        if path is None:
            self._send_text(404, "Not found\n")
            return
        try:
            data = path.read_bytes()
        except OSError as exc:
            self._send_text(500, "could not read file: {}\n".format(exc))
            return
        content_type = _mime_for(path)
        cache = "no-store" if content_type.startswith("text/html") else "no-cache"
        self._send_bytes(200, data, content_type, cache)

    def _read_json_body(self):
        """Read and parse the request body.  Returns None after responding."""
        header = self.headers.get("Content-Length")
        if header is None:
            self.close_connection = True
            self._send_json(
                411, {"ok": False, "error": "Content-Length is required"}
            )
            return None
        try:
            length = int(header)
        except (TypeError, ValueError):
            self.close_connection = True
            self._send_json(
                400, {"ok": False, "error": "invalid Content-Length"}
            )
            return None
        if length < 0 or length > MAX_BODY_BYTES:
            self.close_connection = True
            # Drain a bounded amount so the client can finish sending and read
            # the 413 response instead of seeing a reset connection.
            remaining = length
            if 0 < remaining <= 32 * 1024 * 1024:
                while remaining > 0:
                    chunk = self.rfile.read(min(65536, remaining))
                    if not chunk:
                        break
                    remaining -= len(chunk)
            self._send_json(
                413,
                {
                    "ok": False,
                    "error": "request body exceeds the 4 MB limit",
                },
            )
            return None
        raw = self.rfile.read(length)
        if len(raw) != length:
            self.close_connection = True
            self._send_json(
                400, {"ok": False, "error": "incomplete request body"}
            )
            return None
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            self._send_json(
                400, {"ok": False, "error": "request body is not valid UTF-8"}
            )
            return None
        try:
            return json.loads(text)
        except ValueError:
            self._send_json(
                400, {"ok": False, "error": "request body is not valid JSON"}
            )
            return None

    def _handle_post(self) -> None:
        path = urlsplit(self.path).path
        if path != "/api/analyze":
            if path.startswith("/api/"):
                self._send_json(
                    404,
                    {"ok": False, "error": "unknown API endpoint: " + path},
                )
            else:
                self._send_text(405, "Method not allowed\n")
            return

        body = self._read_json_body()
        if body is None:
            return
        if not isinstance(body, dict):
            self._send_json(
                400, {"ok": False, "error": "request body must be a JSON object"}
            )
            return

        try:
            source = body.get("source")
            if not isinstance(source, str):
                raise ValueError("source must be a string")
            if _utf8_size(source) > MAX_SOURCE_BYTES:
                raise ValueError("source exceeds the 2 MB limit")

            kind = body.get("kind")
            if kind is None:
                filename_hint = body.get("filename")
                kind = (
                    "tdg"
                    if isinstance(filename_hint, str)
                    and filename_hint.lower().endswith(".json")
                    else "rtsl"
                )
            if not isinstance(kind, str) or kind not in KIND_VALUES:
                raise ValueError("kind must be 'rtsl' or 'tdg'")

            mode = body.get("mode", "analyze")
            if not isinstance(mode, str) or mode not in MODE_VALUES:
                raise ValueError("mode must be 'check' or 'analyze'")
            if mode == "check" and kind != "rtsl":
                raise ValueError("check mode only supports RTSL sources")

            response = run_analysis(
                source,
                body.get("filename"),
                kind,
                mode,
                body.get("options"),
            )
        except ValueError as exc:
            self._send_json(400, {"ok": False, "error": str(exc)})
            return
        except BinaryMissingError as exc:
            self._send_json(503, {"ok": False, "error": str(exc)})
            return

        self._send_json(200, response)


class PTPNServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, server_address, handler_class, demo=False):
        self.demo_mode = bool(demo)
        super().__init__(server_address, handler_class)


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------


def _open_browser(url: str) -> None:
    try:
        webbrowser.open(url)
    except Exception:
        pass


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Local web server for the PTPN analyzer",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="bind address (default: 127.0.0.1; localhost only)",
    )
    parser.add_argument(
        "--port", type=int, default=8765, help="listen port (default: 8765)"
    )
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="do not open a browser window automatically",
    )
    parser.add_argument(
        "--demo",
        action="store_true",
        help="serve webapp/demo/index.html and the generated demo data",
    )
    args = parser.parse_args(argv)

    status = get_status(demo=args.demo)

    try:
        httpd = PTPNServer(
            (args.host, args.port), PTPNRequestHandler, demo=args.demo
        )
    except OSError as exc:
        print(
            "error: cannot listen on {}:{}: {}".format(args.host, args.port, exc),
            file=sys.stderr,
        )
        return 1

    bound_host, bound_port = httpd.server_address[:2]
    display_host = "127.0.0.1" if bound_host in ("0.0.0.0", "::", "") else bound_host
    url = "http://{}:{}/".format(display_host, bound_port)

    print("PTPN web UI" + (" (demo mode)" if args.demo else ""))
    print("  URL:      " + url)
    print(
        "  Binary:   "
        + status["binary"]
        + ("" if status["binary_exists"] else "  [MISSING]")
    )
    print(
        "  Graphviz: "
        + (
            status["graphviz"]
            if status["graphviz"]
            else "not found (graphs render without layout)"
        )
    )
    print("  Repo:     " + status["repo_root"])
    print(
        "  Demo:     "
        + (
            "webapp/demo/index.html"
            if args.demo
            else "off (use --demo for the precomputed showcase)"
        )
    )
    if not status["binary_exists"]:
        print(
            "WARNING: the ptpn binary is missing; analysis requests will fail. "
            "Build the project or set PTPN_BIN.",
            file=sys.stderr,
            flush=True,
        )
    print("Press Ctrl+C to stop.", flush=True)

    if not args.no_browser:
        timer = threading.Timer(0.6, _open_browser, args=(url,))
        timer.daemon = True
        timer.start()

    try:
        httpd.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        print("\nStopping PTPN web UI...", file=sys.stderr, flush=True)
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
