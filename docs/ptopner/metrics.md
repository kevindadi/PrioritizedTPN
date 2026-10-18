# PTPN Performance Metrics

This document describes the performance-metrics layer built on top of the
state-class reachability graph (SCG). It explains the one graph change the layer
relies on (per-edge dwell-time bounds), the task metadata embedded in the net,
the metric definitions and algorithms, the JSON output, and the soundness
conventions.

## How to run

```bash
./build/ptpn tdg  -f example/common/input.json --export-metrics metrics.json
./build/ptpn ptpn -f model.ptpn               --export-metrics metrics.json
```

`--export-metrics` implies analysis (the SCG is built even if `--export-scg` is
not given). Metrics are sound only under `--canonicalization equality` (the
default); other modes merge state classes and the tool prints an
"approximate" warning.

## The single graph change: per-edge dwell time

Each SCG edge (`state_class::FiringEdge`) now carries a global **dwell interval**
`[dwell_min, dwell_max]` in addition to the firing window `[firing_min,
firing_max]` of the fired clock:

- `firing_min/max` is the value of the fired transition's own execution clock
  `h_t` at the instant it fires.
- `dwell_min/max` is the wall-clock time the net spends in the **source** state
  class before this firing, i.e. the amount every active clock advances.

`dwell` is computed in `ptpn_analysis.cpp::build()` from the pre-elapse zone
(same layout/index as the time-elapsed zone):

```
hidx        = source.exec_index(t)
entry_low   = -source.zone.get_constraint(0, hidx)   // min h_t on entry
entry_high  =  source.zone.get_constraint(hidx, 0)   // max h_t on entry
dwell_min   = max(0, firing_min - entry_high)
dwell_max   = (firing_max == inf ? inf : max(0, firing_max - entry_low))
```

Dwell is a local, bounded, dedup-safe quantity and is the time weight every
timing metric integrates over. It is also rendered on `state-class-graph.dot`
edges (`dwell=[..]`) and in the SCG JSON export.

## Task metadata in the net

The TDG → PTPN lowering fills `petri::PTPN::task_info` (`TaskInfo`: `core`,
`priority`, `wcet`, `bcet`, `period`, `deadline`, `locks`). `deadline` defaults
to `period` (implicit deadline). A direct `.ptpn` model leaves `task_info`
empty, so only structural and net-level metrics are produced; task-level metrics
are skipped gracefully.

Tasks are mapped to net elements through `node_pn_map[task]`, whose chain
strictly alternates place, transition, place, … (each entry is a tagged
`NodeRef`). From it the analyzer derives each task's `entry` place, `exit`
(`end`) place, chain places (`entry/ready/seg_done/hold/exit`) and execution
transitions (name contains `exec`).

## Metrics

### Tier 0 — structural / correctness

| Metric                         | Definition                                                                  | Algorithm                        |
| ------------------------------ | --------------------------------------------------------------------------- | -------------------------------- |
| Boundedness / max queue        | `max_tokens_per_place[p] = max_v M_v[p]`; compared against capacity         | one pass over vertices, `O(V·P)` |
| Max in-flight (per task)       | max simultaneous tokens across the task's chain places                      | one pass, `O(V·P)`               |
| Deadlock / illegitimate sink   | a vertex with no successor while a task chain place still holds a token     | one pass over sinks              |
| Schedulability                 | no illegitimate deadlock reachable                                          | sink scan                        |

`schedulable = (no illegitimate deadlock)`. Deadline misses are not detected
structurally: the model has no deadline-monitor sub-net. Use the per-task
`slack` (`deadline − WCRT`) instead.

### Tier 1 — timing / response (dwell-weighted, per single activation)

A task **activation** starts when its `entry` place gains a token (a release
edge), or at the initial state for start tasks released at `t = 0`. A task
**completes** on the edge that increments its `end` place.

All response-style metrics are a DP over the **in-flight subgraph** of one task
(vertices where any chain place holds a token), accumulating an edge weight up to
the completion edge:

- **WCRT**: longest path with weight `dwell_max`. A cycle in the in-flight
  subgraph means the job can be delayed forever (identical state class revisited
  while still in flight under `EQUALITY`), so WCRT is reported as `inf`
  (unbounded / starvation).
- **BCRT**: shortest path with weight `dwell_min`, then lifted to `max(path,
bcet)` because the independent per-edge `dwell_min` sum ignores cross-state DBM
  correlation and would otherwise underestimate; `bcet` is an independent sound
  lower bound, and the true BCRT is `≥` both.
- **Jitter**: `WCRT − BCRT`.
- **Worst interference**: longest path accumulating `dwell_max` only over states
  where the task is in the `suspended` set (preempted by higher priority).
- **Worst blocking (priority inversion)**: longest path accumulating `dwell_max`
  over states where the task is in flight, not active, and a strictly
  lower-priority task executes on its core.
- **Max preemptions**: longest path counting edges where the task goes
  `active → suspended`.
- **Slack**: `deadline − WCRT` (negative ⇒ unschedulable).

Each DP is `O(V + E)`; the per-vertex result is memoized.

### Tier 1 — resource (locks)

For each lock resource place (held ⇔ token count is 0):

- **worst_hold**: the longest single-state dwell while the lock is held (longest
  critical-section step).
- **total_hold**: graph-wide sum of held-state dwell (a coarse upper bound, not a
  single-run quantity).
- **total_wait**: held-state dwell during which another task that needs the lock
  is in flight but not active (contention).

### Tier 2 — utilisation / throughput (approximate)

- **Hyperperiod**: `lcm` of all task periods (from `task_info`).
- **Jobs per hyperperiod** (per task): `hyperperiod / period`.
- **Steady cycle**: Tarjan SCC; the largest recurrent SCC reachable from the
  initial state (`has_steady_cycle`, `recurrent_scc_size`).
- **Core utilisation interval** `[util_min, util_max]`: analytic
  `Σ_{task on core} C_i / T_i` using `bcet`/`wcet` — a sound bound independent of
  the graph.
- **graph_busy_fraction**: an approximate state-class average over the recurrent
  SCC — the fraction of (midpoint-dwell-weighted) outgoing transitions whose
  source has an active execution transition on the core. It is an occupancy
  estimate, not a strict time bound.

## JSON output

```json
{
  "exact": true, "states": N, "transitions": M,
  "bounded": true, "schedulable": true,
  "has_steady_cycle": true, "recurrent_scc_size": K, "hyperperiod": H,
  "deadlock_states": [...],
  "tasks": [ { "name", "core", "priority", "wcet", "bcet", "period", "deadline",
               "observed", "activations", "wcrt", "bcrt", "jitter",
               "worst_interference", "worst_blocking", "max_preemptions",
               "max_in_flight", "slack", "jobs_per_hyperperiod" } ],
  "locks": [ { "name", "worst_hold", "total_hold", "total_wait" } ],
  "cores": [ { "core", "util_min", "util_max", "graph_busy_fraction" } ]
}
```

Unbounded time values (`inf`) are serialised as JSON `null`. `max_preemptions`
is `-1` when unbounded.

## Soundness conventions

- Dwell bounds are per-state-class sound upper/lower bounds; they ignore
  cross-state DBM correlation (the standard SCG over-approximation), so WCRT is a
  sound upper bound and BCRT (after the `bcet` lift) a sound lower bound.
- Response-style metrics are scoped to a **single activation** so periodic cycles
  do not produce spurious unbounded longest paths; a genuine in-flight cycle is
  reported as unbounded.
- Task-level metrics require `EQUALITY` canonicalization and TDG-sourced
  `task_info`. Lock `total_hold`/`total_wait` and `graph_busy_fraction` are
  explicitly coarse aggregates / approximations.
