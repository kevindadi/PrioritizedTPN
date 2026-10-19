# Structured diagnostics protocol

PTPN emits a versioned JSON diagnostics envelope for machine consumers (LLM
tools, scripts). The protocol is opt-in: without `--diagnostics` /
`--diagnostics-file` the CLI keeps its historical streams and exit codes.

## Engagement

The envelope and the exit-code mapping below apply **only** when one of these
options is passed to `ptpn tdg`, `ptpn ptpn`, or `ptpn allocate`:

| Option | Effect |
|---|---|
| `--diagnostics json` | Write the envelope to **stdout**. Human-readable logs (spdlog, including `[STATS]` lines) move to stderr, and any direct stdout writes from the pipeline are redirected there as well. |
| `--diagnostics-file PATH` | Write the same envelope to `PATH`. |
| both | Both destinations receive the envelope. |

Without these options the collector is not wired into the pipeline and
behavior is unchanged: logs go to their usual stream, `[STATS]` lines keep
their format, and exit codes are the legacy ones (`0` success, `1` error).

A top-level `try`/`catch` in `main` is always active. An uncaught
`std::exception` prints `ERROR: <what>` to stderr and exits `1`; with the
diagnostics engaged it is additionally reported as `E-INT-001`.

## Envelope

```json
{
  "protocol": "ptpn-diagnostics",
  "version": 1,
  "ok": false,
  "exit_code": 3,
  "diagnostics": [
    {
      "code": "E-VAL-014",
      "category": "validation",
      "severity": "error",
      "location": { "file": "model.json", "line": 7, "column": 3, "span": 9 },
      "subject": "task:fusion",
      "message": "Node fusion uses invalid lock prefix 'foo': must start with 'mutex' or 'spin'",
      "fix": "rename the lock to start with 'mutex' or 'spin'"
    }
  ],
  "truncated_diagnostics": false,
  "result": { "analysis": { } }
}
```

| Field | Meaning |
|---|---|
| `protocol` | Always `"ptpn-diagnostics"`. |
| `version` | Protocol version (`1`). |
| `ok` | `true` iff the computed exit code is `0`. |
| `exit_code` | Priority summary of `diagnostics` (see below), and the process exit code. |
| `diagnostics` | At most 10 entries, in emission order. |
| `truncated_diagnostics` | `true` when more diagnostics existed than fit under the cap. |
| `result` | Omitted when empty. Holds analysis facts (`result.analysis`). |

Each diagnostic entry:

| Field | Meaning |
|---|---|
| `code` | Stable identifier (tables below). Safe to branch on. |
| `category` | `syntax`, `validation`, `unsupported`, `performance`, `analysis`, or `internal` (derived from `code`). |
| `severity` | `error`, or `warning` for performance diagnostics. Validation warnings keep `warning`. |
| `location` | Omitted entirely when unknown. `line`/`column` are 1-based; `span` is a character count. A location may contain only `file` (`.ptpn` parser has no position tracking). |
| `subject` | Optional structured key: `task:<id>`, `lock:<name>`, `node:<id>`, `edge:<src>-><dst>`, `place:<id>`, `transition:<id>`, `arc:<src>-><dst>`, `config:<key>`, `policy:<name>`, `output:<path>`, `system:<aspect>`. Omitted when not applicable. |
| `message` | Human-readable description. Text is not part of the stable contract. |
| `fix` | Optional actionable suggestion for the model author. |

## Categories and exit codes

| Prefix | Category | Default severity | Exit code |
|---|---|---|---|
| `E-SYN-` | syntax | error | 2 |
| `E-VAL-` | validation | error (warnings allowed) | 3 |
| `E-UNS-` | unsupported | error | 4 |
| `E-PERF-` | performance | warning | 5 |
| `R-ANA-` | analysis | error | 6 |
| `E-INT-` | internal | error | 1 |

When categories co-occur, the highest-priority category wins:

```
2 (syntax) > 3 (validation) > 4 (unsupported) > 6 (analysis) > 5 (performance) > 0
```

Internal diagnostics (`E-INT-`) map to `1` and dominate the summary: a bug in
the analyzer is never reported as a clean model-level verdict. Validation
warnings alone do not change the exit code; performance diagnostics always
contribute `5` because they mean the result is partial.

### Analysis-phase rules

The analysis phase follows two rules that keep callers from "fixing" a proven
negative result by raising the state bound:

- A **counterexample found in the explored prefix** is decisive even when the
  graph was truncated: a reachable deadlock state (`R-ANA-001`), a periodic
  local deadlock (`R-ANA-003`), or a certified per-task lower bound exceeding
  the deadline (`R-ANA-002`) is valid. The exit code is `6`.
- Truncation **without** a counterexample (`E-PERF-001` max-states,
  `E-PERF-002` horizon) leaves only partial results. The exit code is `5`, and
  the `fix` text reads `no counterexample found; raising -m may help`.
- The upper-bound `wcrt` exceeding a deadline is **not** a counterexample and
  never produces exit `6`.

`result.analysis` reports the supporting facts:

| Field | Meaning |
|---|---|
| `exact` | Whether all timing values are exact (no abstraction/truncation/symmetry). |
| `truncated` | Whether max-states or horizon truncation occurred. |
| `max_states` | The configured `-m` bound. |
| `horizon_truncated` | Whether the `--horizon` bound pruned expansions. |
| `states`, `transitions` | Explored state-class graph size. |
| `deadlock` | Whether a certified reachable deadlock exists. |
| `deadlock_states` | Number of deadlock state classes. |
| `schedulable` | Certified schedulability, or `null` when uncertified. |
| `local_deadlocks` | Task names in periodic local deadlock. |
| `deadline_misses` | Array of `{task, wcrt_lb, deadline}` counterexamples. |
| `counterexample` | `"deadlock"`, `"local-deadlock"`, `"deadline"`, or `null`. |

## Code tables

### Syntax (`E-SYN-`, exit 2)

| Code | Trigger | Fix template |
|---|---|---|
| `E-SYN-001` | TDG JSON is not valid JSON (nlohmann `parse_error`); location points at the byte offset. | `fix the JSON syntax near the reported line and column` |
| `E-SYN-002` | Input file cannot be opened. | `check that the path exists and is readable` |

The `.ptpn` parser is deliberately lenient and does not emit grammar-level
syntax codes; malformed input either fails validation (`E-VAL-1xx`) or yields
an empty model. `E-SYN-002` applies to both input formats.

### Validation — TDG JSON (`E-VAL-0xx`, exit 3)

Emitted in reading order by `Parser::validate()`: configuration, nodes, edges,
start/end/periodic bindings.

| Code | Trigger | Fix template |
|---|---|---|
| `E-VAL-001` | `task_place_capacity < 1`. | `set configuration.task_place_capacity to 1 (one in-flight job per task)` |
| `E-VAL-002` | `num_cpus < 1`. | `set configuration.num_cpus to at least 1` |
| `E-VAL-003` | `cores_per_cpu < 1`. | `set configuration.cores_per_cpu to a positive value (1 is the supported value)` |
| `E-VAL-004` | Duplicate node id. | `rename one of the duplicate nodes or remove the duplicate` |
| `E-VAL-005` | Unknown node type. | `use one of: task, fork, join, empty` |
| `E-VAL-006` | Task `core` outside `[0, num_cpus-1]`. | `set core to an integer in [0, num_cpus-1] (or remove it to use core 0)` |
| `E-VAL-007` | Negative deadline. | `set deadline to a non-negative value or remove the field` |
| `E-VAL-008` | Empty `allowed_cores` array. | `list at least one core in allowed_cores or remove the field` |
| `E-VAL-009` | `allowed_cores` entry outside `[0, num_cpus-1]`. | `list only cores in [0, num_cpus-1] in allowed_cores` |
| `E-VAL-010` | Duplicate `allowed_cores` entry. | `remove duplicate entries from allowed_cores` |
| `E-VAL-011` | Task `core` not contained in its `allowed_cores`. | `include the task's core in allowed_cores or set core to an allowed one` |
| `E-VAL-012` | Unknown `wcet_kind`. | `use model, certified, measured, or estimated` |
| `E-VAL-013` | Lock not listed in `configuration.shared_locks`. | `add the lock to configuration.shared_locks or remove it from the task` |
| `E-VAL-014` | Lock name without `mutex`/`spin` prefix. | `rename the lock to start with 'mutex' or 'spin'` |
| `E-VAL-015` | Time-interval count differs from `2*lock_count+1`. | `provide 2*lock_count+1 time intervals (pre/CS/post per lock)` |
| `E-VAL-016` | Interval with `min > max`. | `ensure each interval is [min, max] with min <= max` |
| `E-VAL-017` | Fork/join `core` outside `[-1, num_cpus-1]`. | `use -1 (control core) or a core in [0, num_cpus-1]` |
| `E-VAL-018` | Edge source references an unknown node. | `add a node with this id or fix the edge source` |
| `E-VAL-019` | Edge target references an unknown node. | `add a node with this id or fix the edge target` |
| `E-VAL-020` | Start binding references an unknown node. | `reference an existing node id` |
| `E-VAL-021` | Start binding references a non-task node. | `reference a node with type 'task'` |
| `E-VAL-022` | Negative start token count. | `set tokens to a non-negative integer` |
| `E-VAL-023` | End binding references an unknown node. | `reference an existing node id` |
| `E-VAL-024` | End binding references a non-task node. | `reference a node with type 'task'` |
| `E-VAL-025` | Periodic binding references an unknown node. | `reference an existing node id` |
| `E-VAL-026` | Periodic binding references a non-task node. | `reference a node with type 'task'` |
| `E-VAL-027` | Non-positive periodic period. | `set a positive period for the periodic binding` |
| `E-VAL-028` | **Warning:** fork/join declares locks (ignored). | `remove locks from fork/join nodes` |
| `E-VAL-029` | **Warning:** fork/join has multiple intervals, only the first is used. | `keep at most one time interval for a fork/join node` |
| `E-VAL-030` | **Warning:** start task has predecessor edges. | `remove predecessor edges or remove the start binding` |
| `E-VAL-031` | **Warning:** end task has successor edges. | `remove successor edges or remove the end binding` |
| `E-VAL-032` | **Warning:** graph contains no task nodes. | `add at least one node with type 'task'` |
| `E-VAL-033` | JSON structure/type error while reading fields (caught `std::exception`). | `check that every JSON field has the expected type and shape` |

### Validation — `.ptpn` source (`E-VAL-1xx`, exit 3)

The parser stops at the first validation failure (historical behavior). All
locations carry `file` only.

| Code | Trigger | Fix template |
|---|---|---|
| `E-VAL-101` | Place with empty id. | `give every place a non-empty identifier` |
| `E-VAL-102` | Duplicate place id. | `rename one of the duplicate places` |
| `E-VAL-103` | Negative place capacity. | `set the place capacity to a non-negative integer` |
| `E-VAL-104` | Transition with empty id. | `give every transition a non-empty identifier` |
| `E-VAL-105` | Duplicate transition id. | `rename one of the duplicate transitions` |
| `E-VAL-106` | `time_min < 0` or `time_max < time_min`. | `write the interval as [min, max] with 0 <= min <= max` |
| `E-VAL-107` | Empty interval under open bounds. | `use a non-empty interval; open bounds require a strict gap` |
| `E-VAL-108` | Arc source not declared. | `declare the source place or transition before using it in an arc` |
| `E-VAL-109` | Arc target not declared. | `declare the target place or transition before using it in an arc` |
| `E-VAL-110` | Place→place or transition→transition arc. | `connect a place to a transition (pre-arc) or a transition to a place (post-arc)` |
| `E-VAL-111` | Non-positive arc weight. | `set the arc weight to a positive integer` |
| `E-VAL-112` | `@init` references an unknown place. | `reference a declared place in @init` |
| `E-VAL-113` | Negative `@init` token count. | `set the token count to a non-negative integer` |

### Unsupported (`E-UNS-`, exit 4)

| Code | Trigger | Fix template |
|---|---|---|
| `E-UNS-001` | TDG lowering rejects the model as outside a policy's supported class (for example EDF/FIFO/LLF with locks, or PIP/PCP/SRP with more than one in-flight job). Subject is the first offending task/node/edge, or `policy:<name>`. | `simplify the model or choose a supported scheduling policy` |
| `E-UNS-002` | `--romeo`/`--ppn` requested in a build without `PTPN_ENABLE_COMPAT`. | `rebuild with -DPTPN_ENABLE_COMPAT=ON or drop --romeo/--ppn` |

### Performance (`E-PERF-`, exit 5)

| Code | Trigger | Fix template |
|---|---|---|
| `E-PERF-001` | State-class graph hit the `-m` max-states bound. | With a counterexample: `a counterexample was already found; raising -m cannot change the verdict; fix the model design`. Otherwise: `no counterexample found; raising -m may help`. |
| `E-PERF-002` | `--horizon` pruned expansions. | Without a counterexample, the `E-PERF-001` text plus `or increase/disable --horizon`; with a counterexample, the design-change text. |

### Analysis (`R-ANA-`, exit 6)

| Code | Trigger | Fix template |
|---|---|---|
| `R-ANA-001` | Certified reachable deadlock (illegitimate sink). Subject `system:deadlock`. | `fix the dependency/lock structure or choose a different policy` |
| `R-ANA-002` | Certified per-task `wcrt_lb` exceeds its deadline. Subject `task:<name>`. | `adjust priorities, cores, or periods so the task can meet its deadline` |
| `R-ANA-003` | Periodic local deadlock. Subject `task:<name>`. | `change the task's period, priority, or core assignment so it can complete` |

### Internal (`E-INT-`, exit 1)

| Code | Trigger | Fix template |
|---|---|---|
| `E-INT-000` | Fallback for a diagnostic added with an empty code. | — |
| `E-INT-001` | Uncaught exception at the CLI boundary, or defensive catch in the `.ptpn` parser. | `report this as an internal error` |
| `E-INT-002` | Output file could not be written (DOT, WCET JSON, metrics, allocation report, diagnostics file). Subject `output:<path>`. | `check that the output path is writable` |
| `E-INT-003` | Unexpected exception during TDG→PTPN lowering that is not a supported-class guard. | `report this as an internal error` |

## Versioning policy

- Consumers must ignore unknown fields and unknown diagnostic codes.
- Adding new fields, new diagnostic codes, or new optional `result` members
  keeps `version` at `1`.
- Removing or renaming a field, changing the meaning of an existing code or
  field, or changing the exit-code mapping requires bumping `version` to `2`.
- `message` and `fix` texts are informational and may change within a
  version; branch on `code`, `category`, `subject`, and `exit_code`.

## Current scope and deferrals

- Phase 1 wires the existing TDG JSON and `.ptpn` input paths and the
  state-class analysis signals. The RTSL/DSL parser (Phase 2) will reuse the
  same collector, including `mark_tainted()` cascade suppression and the
  `SourceMap`-based location backfill.
- `Diagnostics::mark_tainted()` exists and is honoured by `add()`, but no
  Phase 1 producer marks subjects tainted.
- The `allocate` subcommand accepts both transport flags and reports input
  errors and report-write failures; allocation search truncation is not yet
  classified as `E-PERF` because the search report does not expose a
  per-candidate counterexample/truncation pairing.
