# PTPN JSON Input Format

This document defines the JSON format used to describe a task dependency graph (TDG) for PTPN lowering and analysis.

## Top-level structure

```json
{
  "graph": {
    "name": "TaskGraph"
  },
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 4,
    "shared_locks": ["mutex1", "spin1"],
    "policy": "fixed",
    "start": [{ "task": "A", "tokens": 1 }],
    "end": ["C"],
    "periodic": [{ "task": "A", "period": 100 }]
  },
  "nodes": [],
  "edges": []
}
```

| Field           | Type   | Required | Description                |
| --------------- | ------ | -------- | -------------------------- |
| `graph`         | object | no       | Graph metadata             |
| `graph.name`    | string | no       | Graph name                 |
| `configuration` | object | yes      | System-level configuration |
| `nodes`         | array  | yes      | Node list                  |
| `edges`         | array  | yes      | Edge list                  |

## `configuration`

```json
"configuration": {
  "num_cpus": 2,
  "cores_per_cpu": 4,
  "shared_locks": ["mutex1", "spin1"],
  "policy": "fifo",
  "task_place_capacity": 1,
  "start": [{"task": "A", "tokens": 1}],
  "end": ["C"],
  "periodic": [{"task": "A", "period": 100}]
}
```

| Field                 | Type    | Required | Description                                                                         |
| --------------------- | ------- | -------- | ----------------------------------------------------------------------------------- |
| `num_cpus`            | integer | yes      | Number of real core resources (valid task core ids are `0 .. num_cpus-1`)            |
| `cores_per_cpu`       | integer | yes      | Per-core capacity parameter; values > 1 are not modeled as extra cores (see below)   |
| `shared_locks`        | array   | yes      | Global lock names used by task nodes                                                |
| `policy`              | string  | no       | Scheduling policy, default is `fixed`                                               |
| `task_place_capacity` | integer | no       | Capacity of every task-chain place (entry/ready/segment/hold/exit); default `1`. See below. |
| `start`               | array   | no       | Start task bindings; each item is either a task name or `{ "task", "tokens" }`      |
| `end`                 | array   | no       | End task names; a zero-time consume transition is added after task completion       |
| `periodic`            | array   | no       | Configuration-driven periodic release bindings; each item is `{ "task", "period" }` |

### `task_place_capacity` and saturating semantics

Task-chain places produced by the TDG lowering (entry, ready, per-segment,
lock-hold, and exit places) use **saturating** capacity semantics:

- A transition producing into a full task-chain place is **not** disabled; it
  fires normally and the place's token count is clamped at
  `task_place_capacity` (the overflow is absorbed, not an error).
- This matches single-server semantics: a periodic release that arrives while a
  previous instance is still pending is merged with it instead of blocking the
  release clock (`{task}_fire` keeps firing every `period` without drift).
- With the default value `1`, at most one pending instance is tracked per
  place; a value of `2` additionally tracks one queued release.
- The value must be `>= 1`.
- `pip`, `pcp`, and `srp` support only the default value `1`: lowering rejects
  these policies with `task_place_capacity != 1` (and with start bindings
  requesting more than one token for a task), because queued job tokens would
  make the chain-position ownership inference ambiguous.

Resource places are **not** saturating: core places (capacity =
`cores_per_cpu`), lock places (capacity 1), and the periodic control place
`{task}_period` keep the strict blocking semantics, since dropping a resource
token would be unsound.

Note: the saturation flag is an analysis-time semantic and is not carried by
the Romeo CTS / PToPNer `.ppn` exports; those exports only see the plain
capacity value.

### Scheduling policies

Accepted values:

- `fixed` (alias of resume-style fixed priority)
- `fixed_prior_with_resume`
- `fixed_prior_with_restart`
- `rm`
- `dm`
- `edf`
- `llf`
- `fifo`
- `pip`
- `pcp`
- `srp`

Analysis coverage in this phase:

- `fixed`, `fixed_prior_with_resume`, and `fixed_prior_with_restart` use the
  explicit `priority` values from the input.
- `rm` and `dm` compute static priorities at lowering time (RM: shortest period
  wins; DM: shortest explicit deadline, falling back to the period). For these
  two policies the JSON `priority` field is **ignored**. They use the same
  resume-style preemption and resource lowering as `fixed`; see
  `docs/ptopner/scheduling-semantics.md`.
- `pip` implements the Priority Inheritance Protocol. Each task starts from its
  input `priority`, and the analysis engine dynamically raises a lock holder's
  effective priority to that of its highest-priority waiter (transitively,
  through nested locks) while the waiter is blocked. Ownership is derived from
  the chain position of the task's single job token: lock `i` is held while the
  token lies between the acquire position `5 + 4*i` and the release transition
  position `chain.size() - 4 - 2*i`, and a task waits when its token is on the
  acquire pre-place and the acquire is disabled. Preemption is resume-style and
  lowering matches the other resume policies (no structural `coreN` place,
  suspendable execution segments). `pip` requires `task_place_capacity == 1`;
  lowering rejects larger values. States with zero or multiple tokens in a task
  chain, or inconsistent lock ownership, are outside the supported class and
  fall back defensively (no inheritance for the affected task/lock). See
  `docs/ptopner/scheduling-semantics.md`.
- `pcp` implements the Priority Ceiling Protocol using the input `priority`
  values unchanged (it does **not** combine with the RM/DM priority
  computation in this phase). `ceiling(L)` is the maximum base priority among
  the tasks whose `locks` contain `L`; a lock no task uses has no ceiling.
  Held locks use the same chain-position rule as `pip`. The system ceiling of
  task `J` is the maximum ceiling of locks held by tasks other than `J` (own
  locks excluded; empty means admission is always allowed, with no `0`
  sentinel). `J` may acquire a free lock `L` only when
  `base_priority(J) > system_ceiling(J)`; a lock held by another task is a
  direct block (already structurally disabled) rather than an admission
  decision. Direct requests and conditioned free requests create wait edges
  `J -> holder`; when several held locks attain the system ceiling, the
  lexicographically smallest lock name and its unique holder are selected
  deterministically. Effective priorities are the least fixpoint of
  `eff[holder] = max(eff[holder], eff[waiter])`. Nested locks are permitted.
  Preemption is resume-style and lowering matches the other resume policies
  (no structural `coreN` place, suspendable execution segments). `pcp`
  requires `task_place_capacity == 1` and at most one start token per task;
  lowering rejects larger values. Unsupported or inconsistent markings (zero
  or multiple tokens in a chain, multiple claimants for one lock) fall back
  defensively without inheritance; no WCRT upper-bound guarantee is claimed
  for those states. See `docs/ptopner/scheduling-semantics.md`.
- `srp` implements the Stack Resource Policy using the input `priority` values
  unchanged (no combination with the RM/DM priority computation): the
  preemption level and scheduling priority are both the input `priority`.
  `ceiling(L)` is the maximum base priority among the tasks whose `locks`
  contain `L`; a lock no task uses has no ceiling. The system ceiling at a
  marking is the maximum ceiling over **all** currently held locks (globally
  across the net, including the candidate's own locks); it is empty when no
  lock is held, with no `0` sentinel. Only the task-chain `get_core`
  transition is admission-gated: it is admissible only while the job token is
  at the chain entry and either no lock is held or
  `base_priority(task) > system_ceiling` (strict). A task that has started is
  never blocked again, and lock acquisitions are not admission-tested. There
  is no priority inheritance: effective priorities stay static. Preemption is
  resume-style and lowering matches the other resume policies (no structural
  `coreN` place, suspendable execution segments). `srp` requires
  `task_place_capacity == 1` and at most one start token per task; lowering
  rejects larger values. Unsupported or ambiguous markings skip the start
  gate defensively and are not certified SRP-exact; no WCRT upper-bound
  guarantee is claimed for them. The ceiling is global on multi-core nets (a
  conservative variant of classical uniprocessor SRP). See
  `docs/ptopner/scheduling-semantics.md`.
- `edf`, `llf`, and `fifo` are accepted by the parser but are **not
  implemented** as scheduling semantics. They fall back to the static
  transition priorities with non-preemptive handling and must not be treated as
  analyzed.

## `nodes`

Supported node types:

| Type    | Description                      | Required fields                                     |
| ------- | -------------------------------- | --------------------------------------------------- |
| `task`  | Executable task node             | `id`, `type`, `time`, `locks` (optional: `priority`, `core`, `deadline`, `allowed_cores`, WCET provenance) |
| `fork`  | Fork node (a control transition) | `id`, `type` (optional: `time`, `core`, `priority`) |
| `join`  | Join node (a control transition) | `id`, `type` (optional: `time`, `core`, `priority`) |
| `empty` | Empty node                       | `id`, `type`                                        |

### Task fields

| Field           | Type             | Required | Description                                                                 |
| --------------- | ---------------- | -------- | --------------------------------------------------------------------------- |
| `id`            | string           | yes      | Unique node identifier                                                      |
| `type`          | string           | yes      | Node type                                                                   |
| `priority`      | integer          | no       | Task priority, default `100`; ignored by `rm`/`dm`, which compute priorities |
| `core`          | integer          | no       | Assigned core id, starting from `0`; default `0`                            |
| `deadline`      | integer          | no       | Absolute deadline in the `time` unit; `0`/absent means implicit (= period). DM uses it as its priority key when `> 0`. |
| `allowed_cores` | array of integers | no       | Core-allocation domain; see below. Absent means the task is fixed to `core` |
| `wcet_kind`     | string           | no       | WCET provenance class, default `model`; see below                           |
| `wcet_method`   | string           | no       | Free-form provenance, e.g. `OTAWA`, `MBPTA`, `llm-estimate`                 |
| `wcet_artifact` | string           | no       | Hash or path of the binary/source the WCET refers to                        |
| `time`          | array            | yes      | Execution time segments                                                     |
| `locks`         | array            | yes      | Lock list                                                                   |

### Core-allocation domains

`allowed_cores` declares the set of real cores a task may be allocated to. It
must be a non-empty array of distinct integers in `0 .. num_cpus - 1`.
`cores_per_cpu` is a per-core capacity parameter; it does not create additional
core ids (multi-core parallelism per CPU is intentionally not modeled).

- If `allowed_cores` is absent, the domain is the singleton `{core}` (and
  `core` defaults to `0`), preserving the original fixed-core semantics.
- If `allowed_cores` is present and `core` is absent, `core` is initialized to
  `allowed_cores[0]`.
- If both are present, `core` must be an element of `allowed_cores`.

The domain is consumed by the `ptpn allocate` subcommand, which evaluates
candidate assignments with the exact state-class analysis. Normal `tdg`/`ptpn`
runs still use only the single `core` value; they do not search the domain.

### WCET provenance and trust boundary

`wcet_kind` classifies where the task's WCET came from:

| Kind        | Meaning                                                       |
| ----------- | ------------------------------------------------------------- |
| `model`     | Default; derived from the input time intervals                |
| `certified` | Backed by a certified WCET analysis                           |
| `measured`  | Measured on hardware or a simulator                           |
| `estimated` | Estimated (e.g. by an LLM or a heuristic)                     |

Only `certified` is a certification. `measured`, `estimated`, and `model` WCETs
must not be reported as certified. The WCET export's `all_certified` flag is
`true` only when every task has kind `certified`; a graph with no provenance
fields reports `model` for every task and therefore `all_certified: false`.

`wcet_method` and `wcet_artifact` are optional free-form strings for audit
trails; they do not change analysis semantics.

### Fork / Join fields

Fork and Join nodes are lowered to PTPN **transitions**. By default they are
zero-time control transitions on the control core (`-1`) with priority `0`, but
each of the following may be overridden:

| Field      | Type    | Default    | Description                                                                                                          |
| ---------- | ------- | ---------- | -------------------------------------------------------------------------------------------------------------------- |
| `time`     | array   | `[[0, 0]]` | Firing interval; only the first `[min, max]` segment is used                                                         |
| `core`     | integer | `-1`       | `-1` = control core (not scheduled); `0..N-1` places the sync on a real CPU (subject to per-core priority filtering) |
| `priority` | integer | `0`        | Priority used by the per-core scheduler when `core >= 0`                                                             |

`locks` are meaningless on fork/join nodes and are ignored (a warning is
emitted).

## Lock naming

Locks are distinguished by prefix:

| Prefix  | Meaning    | Examples                 |
| ------- | ---------- | ------------------------ |
| `mutex` | Mutex lock | `mutex1`, `mutex_global` |
| `spin`  | Spin lock  | `spin1`, `spin_irq`      |

Every lock name must start with `mutex` or `spin`.

## Time segment rule

The number of time segments must satisfy:

```text
segment_count = 2 * lock_count + 1
```

Examples:

| Lock count | Segment count |
| ---------- | ------------- |
| 0          | 1             |
| 1          | 3             |
| 2          | 5             |
| n          | 2n + 1        |

This assumes nested locking order.

## `edges`

```json
"edges": [
  {"source": "TaskA", "target": "TaskB", "label": "2,5"},
  {"source": "TaskA", "target": "TaskA", "label": "100"},
  {"source": "TaskA", "target": "TaskC", "label": "50", "style": "dashed"}
]
```

| Field    | Type   | Required | Description                     |
| -------- | ------ | -------- | ------------------------------- |
| `source` | string | yes      | Source node id                  |
| `target` | string | yes      | Target node id                  |
| `label`  | string | no       | Time interval / period (below)  |
| `style`  | string | no       | Optional edge style             |

### Edge `label` semantics

Each ordinary `task -> task` edge is realised by a dedicated **bridge
transition** (`<source>_to_<target>`, a control transition on core `-1`). Its
firing interval comes from the edge `label`:

| Label form     | Interval | Meaning                                                                     |
| -------------- | -------- | --------------------------------------------------------------------------- |
| omitted / `""` | `[0, 0]` | Immediate transfer                                                          |
| `"a"`          | `[a, a]` | Fixed delay                                                                 |
| `"a,b"`        | `[a, b]` | Delay in range; `b` may be `inf` / `+inf` / `∞` / `*` for an open upper end |

Surrounding `[]` / `()` brackets are tolerated. A malformed label falls back to
`[0, 0]` with a warning.

Special cases:

- If the edge `source` **or** `target` is a `fork`/`join`, the edge wires
  directly into that fork/join transition and the `label` is **ignored**
  (put timing on the fork/join node's `time` field instead).
- A **self-loop** edge (`source == target`) is allowed as an annotation but is
  **ignored** by lowering: the model captures task dependencies and task
  attributes, not monitor sub-nets. Periodic activation comes from the
  `periodic` configuration entry (or a dashed edge).
- A **dashed** edge (`"style": "dashed"`) is a periodic release binding; its
  label is not used as a transition interval.

## Example

```json
{
  "graph": {
    "name": "RealTimeTaskSystem"
  },
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 4,
    "shared_locks": ["mutex1", "spin1"],
    "policy": "fifo",
    "start": [{ "task": "TaskA", "tokens": 1 }],
    "end": ["TaskC"],
    "periodic": [{ "task": "TaskA", "period": 100 }]
  },
  "nodes": [
    {
      "id": "TaskA",
      "type": "task",
      "priority": 97,
      "core": 0,
      "time": [
        [0, 10],
        [10, 15],
        [15, 30]
      ],
      "locks": ["mutex1"]
    },
    {
      "id": "TaskB",
      "type": "task",
      "priority": 98,
      "core": 1,
      "time": [[0, 5]],
      "locks": []
    },
    {
      "id": "TaskC",
      "type": "task",
      "priority": 99,
      "core": 0,
      "time": [[0, 8]],
      "locks": []
    }
  ],
  "edges": [
    { "source": "TaskA", "target": "TaskB" },
    { "source": "TaskB", "target": "TaskC" }
  ]
}
```

## WCET summary export

`--export-wcet PATH` writes a per-task summary of the parsed TDG:

```json
{
  "all_certified": false,
  "tasks": [
    {
      "id": 0,
      "interval": [4, 7],
      "name": "TaskA",
      "segments": 2,
      "wcet": 7,
      "wcet_artifact": "sha256:abc",
      "wcet_kind": "certified",
      "wcet_method": "OTAWA"
    }
  ]
}
```

- `wcet` is the sum of the upper bounds of all time segments (unchanged from
  earlier versions); `interval` adds the sum of the lower bounds.
- `wcet_kind`, `wcet_method`, and `wcet_artifact` mirror the input provenance
  (defaults: `model`, empty, empty).
- `all_certified` is `true` only when every task's `wcet_kind` is `certified`.

## Validation rules

Errors:

- Duplicate node ids
- Unknown node types
- Invalid core indices
- Empty, duplicate, or out-of-range `allowed_cores`
- `core` not contained in `allowed_cores` when both are present
- Unknown `wcet_kind` values
- Negative `deadline` values
- `num_cpus` or `cores_per_cpu` below 1
- Edges referencing unknown nodes
- Undefined locks
- Invalid lock prefixes
- Invalid time segment counts
- Invalid time ranges
- Unknown or non-task references in `start`, `end`, or `periodic`
- Non-positive `periodic[*].period`
- Negative token counts in `start`
- Invalid `fork`/`join` core (must be `-1` or a valid core index)

Warnings:

- `start` task has predecessor edges
- `end` task has successor edges
- No task nodes in the graph
- `fork` or `join` nodes declare `locks` (ignored)
- `fork` or `join` nodes declare more than one `time` interval (only the first is used)

## Core index range

Valid core ids for tasks are in:

```text
0 .. num_cpus - 1
```

`cores_per_cpu` is a per-core capacity parameter, not a multiplier for core
ids; the lowering creates exactly `num_cpus` real-core resource places.
`fork`/`join` nodes additionally accept `-1` (the control core), which is their
default when `core` is omitted.
