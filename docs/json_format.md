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
| `num_cpus`            | integer | yes      | Number of CPUs                                                                      |
| `cores_per_cpu`       | integer | yes      | Number of cores per CPU                                                             |
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

Resource places are **not** saturating: core places (capacity =
`cores_per_cpu`), lock places (capacity 1), and the periodic control place
`{task}_period` keep the strict blocking semantics, since dropping a resource
token would be unsound.

Note: the saturation flag is an analysis-time semantic and is not carried by
the Romeo CTS / PToPNer `.ppn` exports; those exports only see the plain
capacity value.

### Scheduling policies

Supported values:

- `fixed`
- `rm`
- `dm`
- `edf`
- `llf`
- `fifo`
- `pip`
- `pcp`
- `srp`

## `nodes`

Supported node types:

| Type    | Description                      | Required fields                                     |
| ------- | -------------------------------- | --------------------------------------------------- |
| `task`  | Executable task node             | `id`, `type`, `priority`, `core`, `time`, `locks`   |
| `fork`  | Fork node (a control transition) | `id`, `type` (optional: `time`, `core`, `priority`) |
| `join`  | Join node (a control transition) | `id`, `type` (optional: `time`, `core`, `priority`) |
| `empty` | Empty node                       | `id`, `type`                                        |

### Task fields

| Field      | Type    | Description                         |
| ---------- | ------- | ----------------------------------- |
| `id`       | string  | Unique node identifier              |
| `type`     | string  | Node type                           |
| `priority` | integer | Task priority                       |
| `core`     | integer | Assigned core id, starting from `0` |
| `time`     | array   | Execution time segments             |
| `locks`    | array   | Lock list                           |

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

## Validation rules

Errors:

- Duplicate node ids
- Unknown node types
- Invalid core indices
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
0 .. num_cpus * cores_per_cpu - 1
```

`fork`/`join` nodes additionally accept `-1` (the control core), which is their
default when `core` is omitted.
