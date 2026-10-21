# PTPN Web UI backend

A local web UI for browsing and running PTPN analyses. The backend is a
**zero-third-party-dependency** Python 3.9+ standard-library HTTP server
(`ThreadingHTTPServer`) that wraps the `build/ptpn` command-line analyzer in a
structured JSON API. The front-end is static HTML/CSS/JS under
`webapp/static/`.

```
browser  ──HTTP──▶  webapp/server.py  ──argv──▶  build/ptpn
                          │                         │
                          └── temp dir: model.rtsl / model.json
                              TDG / PTPN / SCG / metrics / WCET exports
```

## Requirements

- Python 3.9 or newer (standard library only; no `pip install`).
- A built PTPN analyzer at `build/ptpn` (see the repository `CLAUDE.md` for
  build instructions).
- Graphviz (optional): `dot` lays out PTPN graphs and `neato` lays out SCG
  graphs. If `neato` is missing, the server falls back to `dot`. Without
  Graphviz the API still works; graphs are returned without coordinates
  (`"layout": "none"`) and a notice is added.
- A modern browser.

## Start

```bash
python3 webapp/server.py
```

The server listens on `127.0.0.1:8765` by default, prints the URLs, and opens a
browser. Press `Ctrl+C` to stop.

- **Story** (default): `/`
- **Workbench**: `/workbench`

```bash
python3 webapp/server.py --port 8799 --no-browser   # test / headless
python3 webapp/server.py --demo                      # precomputed showcase
python3 webapp/build_standalone.py                   # single-file workbench demo
python3 webapp/build_standalone.py --story           # single-file story demo
```

| Option | Default | Description |
|---|---|---|
| `--host HOST` | `127.0.0.1` | Bind address. Keep localhost unless LAN access is intended; see the security notes. |
| `--port PORT` | `8765` | Listen port; pass `0` to let the system assign one. |
| `--no-browser` | off | Do not open a browser automatically. |
| `--demo` | off | Prefer the generated `webapp/demo/demo-data.js` for both pages. Falls back to the empty `webapp/static/demo-data.js` placeholder if the generated file is missing. |

### LAN access

Binding to another interface exposes an unauthenticated analyzer. For a
trusted LAN only:

```bash
python3 webapp/server.py --host 0.0.0.0 --port 8091 --no-browser
```

Use a port that the host firewall already allows, or add a rule for the chosen
port. Do not publish this server to the internet. The startup banner prints the
primary LAN URL when listening on all interfaces.

## Front-end

`webapp/static/` contains two no-build pages (plain HTML/CSS/JS):

### Story (`/`, `story.html` / `story.css` / `story.js`)

A narrative-first explanation of the method:

- the problem chapter: WCET versus WCRT, interval execution times, and residual
  work after preemption;
- the model chapter: TDG → PTPN places/transitions, tokens, periodic release,
  dependency bridges, core/priority attributes, and a physical-object map;
- the firing-rule chapter: a frame-by-frame replay of the analyzer's SCG path
  showing E<sub>struct</sub> / E<sub>pri</sub> / R, DBM clock bounds, the core
  timeline, and F being preempted by B and resuming from `h=1`;
- the policy chapter: fixed / RM / DM / structural restart / EDF / FIFO results
  on the same model, including supported-class rejections;
- the LLM chapter: RTSL / TDG JSON / PTPN front doors, the three timing sources,
  `check` / `fill` / analyze, and a repair prompt built from the verdict and
  witness.

All story markings, sets, clocks, windows and verdicts come from the
`/api/analyze` response (or the precomputed `demo-data.js`). The browser does
not recompute scheduling results.

### Workbench (`/workbench`, `index.html` / `style.css` / `app.js`)

The analysis workbench:

- model list and run options (max states, canonicalization, reduction, policy,
  horizon, extrapolation);
- `Model` tab: RTSL/TDG syntax-highlighted editor, line diagnostics, and a
  one-click form for pending `measured by llm` measurements;
- `TDG` / `PTPN` / `SCG` tabs: pan/zoom graph views with inspectors; PTPN opens
  focused on the first execution chain, and `FIT` reveals the whole net;
- `Overview`: WCRT/WCET envelopes, core utilization, response-ratio dial, lock
  contention, and a diagnostic summary;
- `Metrics`: the full per-task timing table, core/lock tables, global facts, and
  JSON copy actions;
- a bottom `RUN LOG` drawer with the analyzer's stderr and `[STATS]` timings.

The visual language is a graphite/copper/teal precision-instrument style; chart
colors are checked for contrast and state colors are separate from series
colors. Mobile layouts provide a `☰` model drawer.

### Front-end file conventions

- `GET /` (and `/story`) reads `webapp/static/story.html`; `GET /workbench`
  reads `webapp/static/index.html`. Both are authored as **fragments without
  `<!doctype>/<html>/<head>/<body>`**; the server wraps them into full
  documents:

  ```html
  <!doctype html><html><head><meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  </head><body> …fragment… </body></html>
  ```

- Relative references (Story's `story.css` / `story.js`, Workbench's
  `style.css` / `app.js`, and the shared `demo-data.js`) are resolved from
  `webapp/static/` (preferring `webapp/demo/` under `--demo`), so root-level
  paths such as `/story.css` also work.
- `/docs/<file>` serves the repository `docs/` directory read-only; the Story
  footer links use this route.
- `webapp/static/demo-data.js` is an empty placeholder served with the normal
  JavaScript MIME type. Production/artifact demo data is
  `webapp/demo/demo-data.js`.
- API and HTML responses use `Cache-Control: no-store`.

## Binary discovery

1. `PTPN_BIN` (relative paths are resolved from the repository root).
2. The build directory recorded in `.ptpn-build-dir` at the repository root,
   using `<dir>/ptpn`. If that binary is missing, fall through to step 3.
3. `build/ptpn`.

Set `DOT_BIN` to override the Graphviz executable (the default is `dot` from
`PATH`; `neato` is looked up in the same directory or in `PATH`).
`GET /api/status` reports the selected binary, version, Graphviz version and
availability. If the binary is missing, `binary_exists` is `false`,
`binary_error` is included, and analysis requests return HTTP 503.

## JSON API

All responses are UTF-8 JSON. Errors use `{"ok": false, "error": "..."}`.

### `GET /api/status`

```json
{
  "ok": true,
  "demo": false,
  "binary": "/repo/build/ptpn",
  "binary_exists": true,
  "version": "1.0.0",
  "graphviz": "13.0.1",
  "repo_root": "/repo",
  "build_dir": "build"
}
```

### `GET /api/examples`

`{"examples": [ … ]}` contains only examples that exist on disk. Each object:

```json
{
  "id": "intro-demo",
  "name": "demo-filled.rtsl",
  "title": "Sensor fusion pipeline",
  "kind": "rtsl",
  "path": "example/introduction/demo-filled.rtsl",
  "source": "…file contents…",
  "description": "…",
  "tags": ["RTSL", "fork/join"],
  "demo": true,
  "options": { "max_states": 10000, "canonicalization": "equality",
               "reduction": "none", "policy": "", "horizon": null,
               "extrapolation": false, "keep_suspension_clocks": false }
}
```

`deadlock` adds `"render_limit": 500`; `symmetry-4` uses
`"reduction": "symmetry"`; `pipeline-20` uses `"max_states": 20000`;
`deadline-miss` uses `max_states = 10` and returns `R-ANA-002` +
`E-PERF-001` for the Story repair loop.

### `POST /api/analyze`

Request body:

```json
{
  "source": "system demo { … }",
  "filename": "model.rtsl",
  "kind": "rtsl",
  "mode": "analyze",
  "options": {
    "max_states": 10000,
    "canonicalization": "equality",
    "reduction": "none",
    "policy": "",
    "horizon": null,
    "extrapolation": false,
    "keep_suspension_clocks": false,
    "render_limit": 1800
  }
}
```

- `source`: required UTF-8 string, at most 2 MB. **File paths are not accepted.**
  The server always writes it to a temporary `model.rtsl` / `model.json`.
- `kind`: `rtsl` or `tdg`; inferred from `filename` when omitted.
- `mode`: `check` (RTSL only; runs `ptpn check`) or `analyze` (default).
- `options`: whitelisted fields; unknown fields return HTTP 400.
  - `max_states`: 1–200000, default 10000.
  - `canonicalization`: `equality` | `max-lower` | `intersection`.
  - `reduction`: `none` | `upper-bound` | `cone` | `symmetry`.
  - `policy`: policy name (passed to the CLI only when non-empty).
  - `horizon`: non-negative integer or `null`.
  - `extrapolation`, `keep_suspension_clocks`: booleans.
  - `render_limit`: graph render cap, default 1800; layout is skipped above it.

A completed analysis returns HTTP 200 regardless of the analyzer exit code:

```json
{
  "ok": false,
  "exit_code": 6,
  "duration_ms": 88,
  "diagnostics": [ … ],
  "envelope": { … } | null,
  "analysis": { … } | null,
  "pending_measurements": [ … ],
  "metrics": { … } | null,
  "wcet": { … } | null,
  "tdg": { … } | null,
  "graphs": { "ptpn": { … } | null, "scg": { … } | null },
  "timings": { "lowering_ms": 0, "scg_ms": 11,
               "metrics_ms": 4, "total_ms": 59 },
  "logs": ["…last 300 stderr lines, ANSI stripped…"],
  "effective_options": { … },
  "notices": ["…truncated graph, missing dot, timeout…"]
}
```

Diagnostic `location.file` values (and `pending_measurements[].inject.file`)
are rewritten from temporary paths to the client's `filename`.

**Exit codes**: `0` success; `2` syntax error; `3` validation error; `4`
unsupported class; `5` partial results (truncation without a counterexample);
`6` proven deadlock / deadline counterexample (still a valid analysis;
`metrics` is usually present, `ok=false`); `105` is a CLI11 argument error
(no envelope; a notice explains it); `124` is the server's 180-second timeout
after killing the process.

HTTP errors: `400` invalid request/options, `411` missing
`Content-Length`, `413` body over 4 MB, `503` no executable `ptpn`, `500`
internal server error.

### Graph (`graphs`) structure

```json
{
  "nodes": [
    {
      "id": "sensorexec", "kind": "exec", "text": "sensorexec",
      "tooltip": null, "raw_label": "sensorexec\n(core=0, π=100)\nI=[[2, 3]",
      "core": 0, "priority": 100, "interval": [2, 3],
      "x": 693.93, "y": 226.88, "w": 111.13, "h": 61.92
    }
  ],
  "edges": [
    { "from": "sensorentry", "to": "sensorget_core",
      "label": "", "tooltip": null,
      "points": [[211.11, 226.88], …] }
  ],
  "bb": { "w": 3966.2, "h": 510.45 },
  "truncated": false,
  "node_count": 32,
  "edge_count": 34,
  "layout": "neato"
}
```

- Coordinates are points (pt), origin at the top-left (y is flipped using
  the `bb` height).
- PTPN nodes: `circle` is a place (with `marking`, `capacity`), `box` is a
  transition (with `core`, `priority`, `interval`); `kind` is one of
  `place/period/resource/hold/segment/exec/acquire/lock/fork/join/control/transition`.
- SCG nodes: `label` is `S<n>` and `kind` is `state`; when
  `metrics.deadlock_states` contains the state id, `kind` is `deadlock` and
  `deadlock` is always present as a boolean. `detail` is parsed from the
  tooltip:
  `{elapsed, marking, enabled, e_struct, suspended, zone}`, plus the structured
  sets `e_struct_transitions` / `priority_transitions` /
  `suspended_transitions` (each entry has `id`, `name`, `priority`, `core`,
  `suspendable`) and `clocks` (each entry has `name`, `kind`, `transition`,
  `lower`, `upper`). The Story replay consumes these fields directly;
  `enabled` remains the compatible E<sub>pri</sub> string.
- SCG edge labels are parsed into `transition`, `core`, `dwell_min/max` and
  `at_min/max`.
- `edge.points` run from tail to head and omit Graphviz `e,` / `s,` marker
  points.
- Above `render_limit`, layout is skipped: `nodes`/`edges` are empty,
  `"truncated": true`, `"layout": "none"`, and a notice is added.
- If Graphviz fails, the parsed nodes/edges are returned without coordinates,
  `"layout": "none"`, plus a notice.

## Regenerate demo data

```bash
python3 webapp/make_demo.py
```

This imports `server.run_analysis` (without starting HTTP), runs every
`"demo": true` example with its recommended options, and writes
`webapp/demo/demo-data.js`:

```js
window.PTPN_DEMO_DATA = {
  "generated_at": "2026-10-21T06:17:59Z",
  "status": { … /api/status with demo=true … },
  "examples": [ { …example fields…, "source": "…", "response": { … } } ],
  "story": { "policies": { "fixed": {…}, "rm": {…}, … } }
};
```

- `story.policies` stores compact `hw` results under fixed / RM / DM / restart /
  EDF / FIFO (exit code, state count, schedulability, per-task WCRT/verdict,
  diagnostics) for the offline policy chapter.
- The generated file is currently about 3 MB; the hard limit is 8 MB. The script
  errors without writing above the limit.
- Every `</` is escaped as `<\/`, so the payload is safe to embed in HTML;
  `</script>` never appears.
- The script prints each example's exit code, state count, metrics presence,
  SCG/PTPN node/edge counts, and response size.
- Write elsewhere with `python3 webapp/make_demo.py OUTPUT_PATH`.
- Build server-free single-file demos:
  - workbench: `python3 webapp/build_standalone.py` →
    `webapp/demo/ptpn-chronograph-demo.html`;
  - Story: `python3 webapp/build_standalone.py --story` →
    `webapp/demo/ptpn-story-demo.html`.
  Both inline the CSS/JS/precomputed data and can be opened directly or shared.
  Google Fonts are optional; the pages have system fallbacks.
- Validate with `node --check webapp/demo/demo-data.js`.
- The demo data covers only demo examples. If `webapp/demo/demo-data.js` is
  missing, `--demo` falls back to the empty placeholder and the front-end uses
  the live API.

## Security notes

- The server binds to `127.0.0.1` by default and has **no authentication**. Do
  not bind it to `0.0.0.0` or expose it to the internet unless the network is
  trusted.
- Submitted source is parsed by the analyzer (which handles untrusted input) but
  is **never passed through a shell**: the server uses argument arrays with
  `subprocess`, so clients cannot inject commands.
- Clients cannot provide file paths; source is written only to a fresh temporary
  directory with fixed names `model.rtsl` / `model.json`.
- Per-request TDG, PTPN DOT, SCG DOT, metrics and WCET exports are temporary
  files in that directory and are deleted recursively when the request finishes
  (including timeouts and exceptions).
- Limits: 4 MB request body, 2 MB source, 180-second analysis timeout, 60-second
  DOT layout timeout.
- The static file handler resolves real paths and checks `commonpath`, rejecting
  `..`, backslashes, and symlinks that escape the directory. `/docs/` is served
  read-only under the same check for the Story footer links.
