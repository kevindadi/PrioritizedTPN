# Architecture

PTPN is organized as a one-way pipeline:

```text
RTSL source ──► lex / parse / validate ──► TDG ──┐
TDG JSON    ──► parse ───────────────────► TDG ──┴──► tdg2pn ──► PTPN ──┐
PTPN source ──► parse ───────────────────────────────────────► PTPN ────┴──► analysis
                                                                              │
                                                                              ▼
                                                        metrics / diagnostics / DOT
```

The code mirrors that pipeline in static libraries. Each layer declares only
the dependencies it uses. The direction is:

```text
CLI  →  application  →  frontend / model / lowering / analysis / export
```

Nothing below `src/app` includes CLI11 or command registration, and analysis
code does not write the CLI diagnostics envelope.

## Source layout

| Directory | Responsibility |
|---|---|
| `src/common/` | `Diagnostics` collector, envelope serialization, `Result<T>`, `UnsupportedClassError` |
| `src/model/` | TDG and PTPN data structures plus `model/types.h` (task/policy types) |
| `src/dsl/` | RTSL lexer, panic-mode parser, validator, in-process TDG lowering, `SourceMap` |
| `src/parse/` | TDG JSON parser (nlohmann) and `.ptpn` source parser |
| `src/lower/tdg2pn/` | TDG → PTPN lowering (chain construction, preemption, policy priorities) |
| `src/analysis/dbm/` | Difference-bound matrices and clock layout |
| `src/analysis/reachability/` | State classes, firing, canonicalization, scheduling policies (`scheduling/`), symmetry |
| `src/analysis/metrics/` | WCRT/schedulability/deadlock/utilisation metrics computation |
| `src/analysis/reductions/` | Per-task cone projection |
| `src/analysis/allocation/` | Deterministic core-allocation search (evaluates candidates with the exact analysis) |
| `src/export/` | Output writers: TDG DOT, PTPN DOT, state-class DOT, TDG JSON, WCET JSON. Metrics JSON lives next to the metrics library in `src/analysis/metrics/`, so export does not depend on `ptpn_metrics` (the SCG writer still depends on `ptpn_reachability`) |
| `src/app/` | CLI-free pipelines, input loading, analysis orchestration, allocation driver, check command |
| `src/cli/` | CLI11 option registration and dispatch; no domain logic |

## Application layer

The application layer is split by responsibility so a command only compiles and
links what it needs:

| File | Responsibility | Library |
|---|---|---|
| `common.{h,cpp}` | `PipelineOptions`, `ExportTargets`, `DiagnosticsOptions`, legacy diagnostic rendering, process memory, pipeline clock | `ptpn_app_support` |
| `logging.h` | spdlog-backed `[STATS]` timing helper and scoped log-level guard | `ptpn_app_support` |
| `input.{h,cpp}` | Read, lex, parse, validate and lower RTSL; TDG JSON loading and `--policy` override | `ptpn_app_support` |
| `check_cmd.{h,cpp}` | `ptpn check`: parse/validate only; never lowers | `ptpn_app_check` |
| `lowering.{h,cpp}` | TDG→PTPN plus `E-UNS-001`/`E-INT-003` adaptation and SourceMap lookup | `ptpn_app_pipelines` |
| `analysis.{h,cpp}` | Analysis orchestration (cone/full graph, metrics, DOT), result diagnostics | `ptpn_app_pipelines` |
| `pipelines.{h,cpp}` | `run_tdg_pipeline`, `run_rtsl_pipeline`, `run_ptpn_pipeline`; public facade | `ptpn_app_pipelines` |
| `allocation_cmd.{h,cpp}` | `ptpn allocate`: input dispatch, search driver, report summary | `ptpn_app_allocation` |

`check_cmd` depends on `ptpn_app_support` only; it does not link lowering,
analysis, or allocation. `ptpn rtsl` and `ptpn tdg` share `run_tdg_analysis`,
so their analysis and export semantics cannot diverge. `.ptpn` input shares
`run_ptpn_postprocess` with them.

### RTSL input contract

`app/input.h` exposes the boundary explicitly:

- `read_rtsl_source` — file read only, no diagnostics;
- `parse_and_validate_rtsl` — lex/parse/validate and returns a `RtslDocument`
  (program + `SourceMap` + validity); panic-mode parsing still returns a
  program when validation failed so `check` can report task/edge counts;
- `lower_rtsl_document` — runs `dsl::lower` only for a valid document;
- `load_tdg_from_rtsl` — the full path used by `rtsl` and `allocate`, returning
  the TDG and the `SourceMap` needed to locate later `E-UNS-001` diagnostics.

The Check/Rtsl/Allocate distinction is a `dsl::ValidationOptions::Mode` passed
through this API; the stages themselves are shared.

### Diagnostics boundary

`ptpn::Diagnostics` (in `src/common/`) is the data collector: codes, subjects,
locations, cap/taint, exit-code summary, and envelope serialization.
`src/cli/main.cpp` owns the transport: it decides whether the envelope is
engaged, redirects logs to stderr, writes stdout/file, and returns the envelope
exit code. `src/app/` records diagnostics but never chooses a stream or a
process exit code.

### Completion observation (exact WCRT)

The state-class metrics read the worst-case response time from an observation
release clock instead of accumulating per-edge dwell maxima along a path (that
sum ignores cross-state DBM correlation and over-approximates). The reachability
builder adds a scheduling-irrelevant release clock to the layout for the
scalar-priority rules (`FiringRule::supports_completion_observation`), and every
completion edge records the sup of that clock in the fired zone
(`FiringEdge::observed_response`). The clock is capped at the task's deadline + 1
(or a structural fallback): when the sup reaches the cap, the clock is abstracted
to ">= cap, otherwise unconstrained" (a relaxation that keeps every other clock
exact and the zone space finite) and the metrics layer reports the task as
`wcrt_capped` with a certified lower bound — which also proves the deadline miss.
The observation is disabled under extrapolation, upper-bound merging, symmetry,
non-equality canonicalization, `--tie arbitrary`, and per task when a release
overlaps an in-flight job (one column cannot time two jobs).

A transition whose execution clock is pinned at the upper bound of its interval
has finished its work, so `force_completed_transitions` keeps it active and the
next step fires it at the completion instant: a preemption must not suspend a
finished clock and delay the completion. Classic fixed-priority response-time
analysis and a tick-based simulator agree with the reported values on
synchronously released single-core models (`test/test_reference_analysis.cpp`);
the "only part of the class reached the bound" case keeps the previous
suspension semantics and remains a documented over-approximation.

## Build targets

The CMake targets follow the same direction:

```text
ptpn_types, ptpn_diagnostics
        │
ptpn_dsl, ptpn_json, ptpn_tdg, ptpn_petri, ptpn_parser
        │
ptpn_lowering
        │
ptpn_dbm → ptpn_reachability → ptpn_metrics, ptpn_reductions
        │
ptpn_export, ptpn_allocation
        │
ptpn_app_support → {ptpn_app_check, ptpn_app_pipelines, ptpn_app_allocation}
        │
ptpn_cli → ptpn executable
```

`PTPN_core` is an INTERFACE aggregate used by the test executable; it is not a
layer of its own. Application targets link their implementation dependencies as
`PRIVATE` where the public headers only forward-declare them, so include paths
and compile definitions do not leak to unrelated commands.

## Tests

`test/` contains one `test_<concern>.cpp` per concern, linked into a single
`ptpn_test` executable. CLI-level tests run the `ptpn` binary and parse the
diagnostics envelope; `test/cli_test_helpers.h` provides the shared runner,
temporary-file and envelope helpers. Golden files live in `test/golden/` and
are compared only on libstdc++ builds.

## Known structural notes

These are recorded for future work; this repository does not change analysis
semantics to remove them:

- `model/tdg.cpp` still implements TDG JSON *loading* and includes
  `parse/json.h` (which includes `model/types.h`), so the include and link
  dependency runs one way: `ptpn_tdg` privately depends on `ptpn_json`. There is
  no cycle, but the responsibility is inverted: TDG JSON loading conceptually
  belongs to the parse layer. The TDG/PTPN/SCG DOT, TDG JSON and WCET JSON
  writers live in `src/export/`; metrics JSON stays with the metrics library so
  export does not depend on `ptpn_metrics` (the SCG writer still depends on
  `ptpn_reachability`). Loading remains for a follow-up.
- `analysis/allocation/` is grouped under `analysis/` but depends on
  `lower/tdg2pn/` to evaluate candidates. It is orchestration above the
  analysis core.
- `analysis/reachability/scheduling/task_lock_topology.h` mirrors the task-chain layout
  constants defined by `lower/tdg2pn/tdg2pn_common.h`. The analysis layer
  deliberately does not include lowering headers, so the constants are
  duplicated with comments; a shared header would remove the silent-drift risk.
- TDG JSON validation diagnostics have no source locations (the format has no
  positions), and PTPN validation locations are file-only.
- Supported-class rejection is a typed exception (`ptpn::UnsupportedClassError`)
  thrown by lowering and by the verified job-clock/option analysis guards. The
  app maps that type to `E-UNS-001`; metadata/index/clock-layout invariants
  remain ordinary internal exceptions. Do not widen the mapping to every
  `std::invalid_argument`.
- Spin locks keep the CPU: `vertex_lowering.cpp` marks every execution segment
  inside a `spin` critical section non-suspendable, derived from the actual
  chain layout (lock `i` acquired after segment `i`, returned by the segment
  that closes its nested critical section). See `docs/rtsl.md` §7/§12.2 and
  `example/diagnostics/spin-lock.rtsl`.

See [diagnostics.md](diagnostics.md) for the code table and envelope contract,
[rtsl.md](rtsl.md) for the language, and [usage.md](usage.md) for commands and
options.
