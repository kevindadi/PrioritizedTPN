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

# Sound upper bound with one-sided zone merging (fewer states, no wcrt_lb)
./build/ptpn tdg  -f example/l-bench/pipeline-40t-8c.json \
    --reduction upper-bound --export-metrics metrics-upper.json
```

`--export-metrics` implies analysis (the SCG is built even if `--export-scg` is
not given). Metrics are exact only under `--canonicalization equality` (the
default), with no `--extrapolation`, no `--reduction upper-bound`, and without
truncation; the other modes merge or omit state classes and the tool prints an
"approximate" warning.

## Upper-bound zone merging (`--reduction upper-bound`)

The default analysis keeps every distinct state class. The opt-in
`--reduction upper-bound` mode applies a sound **one-sided** abstraction: when a
newly discovered candidate class has the same marking and the same selected
active set `E_pri-hat(M)` as an already-discovered class, and

```
candidate.zone ⊆ existing.zone
```

(`DBM::included_in`), the candidate is merged into the existing (larger) class.
Classes with different markings or different selected active sets are never
merged, because the scheduling semantics differs. The larger zone is kept
unchanged, so every concrete behavior of the candidate is covered by the
existing class and the resulting graph over-approximates the exact state space.

Consequences for the metrics:

- `wcrt` remains a **sound upper bound** on the exact WCRT.
- `wcrt_lb` is **not certified** on this graph: the tighter candidate zones and
  their `dwell_min` bounds were discarded, so a bound derived from the merged
  graph is not a certified lower bound for the original. The analyzer therefore
  reports `wcrt_lb` as JSON `null` and sets the top-level `wcrt_lb_valid` flag
  to `false`. `wcrt_interval_tight` is always `false` in this mode.
- `exact` is `false`.
- `Statistics::zone_merges` counts candidate classes absorbed into a larger
  class; `Statistics::merged_states` counts the distinct existing classes that
  absorbed at least one candidate.

The mode is one-sided by design: if the smaller zone happens to be discovered
first, the later (larger) class is added as a separate vertex; the graph remains
an over-approximation, just with less reduction. `--extrapolation` is an
alternative upper-bound mode that bounds unbounded clocks; the two modes can be
compared on a case-by-case basis, and either may produce fewer states.

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
  (unbounded / starvation). This is a sound **upper bound**: it relaxes the
  cross-edge correlation between entry clock values.
- **WCRT lower bound (`wcrt_lb`)**: longest path with weight `dwell_min`. Every
  edge's dwell is at least `dwell_min`, so this is a sound lower bound on the
  same quantity. The exact WCRT lies in `[wcrt_lb, wcrt]`;
  `wcrt_interval_tight` is true only when the two coincide (a sufficient, not
  necessary, exactness certificate). Under `--reduction upper-bound` this bound
  is not certified and is reported as unavailable (`null`); see above. 
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
- **Deadline miss**: `wcrt > deadline` (using the sound upper bound), or an
  infinite WCRT. `schedulable` is false when any task misses its deadline, when
  a reachable illegitimate sink exists, or when a periodic local deadlock is
  detected.
- **Periodic local deadlock**: for each task, the analysis restricts the SCG to
  classes where the current instance is still pending (a chain token before the
  exit place), removes edges labelled by the task's own periodic release, and
  runs an iterative Tarjan SCC decomposition. A cyclic SCC containing an internal
  edge labelled by a positive periodic release of another task is a periodic
  local deadlock. This is the paper's Algorithm 5; `local_deadlock_scc_size`
  reports the largest such SCC.
- **Slack**: `deadline − wcrt` (negative ⇒ unschedulable upper bound).

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
  "exact": true, "wcrt_lb_valid": true, "states": N, "transitions": M,
  "bounded": true, "schedulable": true,
  "has_steady_cycle": true, "recurrent_scc_size": K, "hyperperiod": H,
  "deadlock_states": [...],
  "tasks": [ { "name", "core", "priority", "wcet", "bcet", "period", "deadline",
               "observed", "activations", "wcrt", "wcrt_lb", "wcrt_interval_tight",
               "bcrt", "jitter", "worst_interference", "worst_blocking",
               "max_preemptions", "max_in_flight", "slack", "deadline_missed",
               "local_deadlock", "local_deadlock_scc_size", "jobs_per_hyperperiod" } ],
  "locks": [ { "name", "worst_hold", "total_hold", "total_wait" } ],
  "cores": [ { "core", "util_min", "util_max", "graph_busy_fraction" } ]
}
```

Unbounded time values (`inf`) are serialised as JSON `null`. `max_preemptions`
is `-1` when unbounded. When `wcrt_lb_valid` is `false`, each task's `wcrt_lb`
is serialised as `null`.

## Soundness conventions

- Dwell bounds are per-state-class sound upper/lower bounds; they ignore
  cross-state DBM correlation (the standard SCG over-approximation), so WCRT is a
  sound upper bound, `wcrt_lb` a sound lower bound, and BCRT (after the `bcet`
  lift) a sound lower bound. The exact value lies in `[wcrt_lb, wcrt]`.
- `exact` is true only when the graph is complete
  (`--canonicalization equality`, no `--extrapolation`, no
  `--reduction upper-bound`, not truncated). With extrapolation,
  canonicalization merging or upper-bound zone merging, timing values are
  bounds and the flag is false.
- Under `--reduction upper-bound`, `wcrt` is a sound upper bound and
  `wcrt_lb_valid` is false. The lower bound is computed only on an exact
  (unmerged) graph.
- Task-level metrics require TDG-sourced `task_info`; a direct `.ptpn` model
  produces structural and net-level metrics only.
- The SCG enumerates every admissible equal-priority active set; worst-case
  WCRT is therefore taken over all tie resolutions. Tie-heavy models can be
  much larger than models with unique priorities per core.
- Response-style metrics are scoped to a **single activation** so periodic cycles
  do not produce spurious unbounded longest paths; a genuine in-flight cycle is
  reported as unbounded.
- Task-level metrics require `EQUALITY` canonicalization and TDG-sourced
  `task_info`. Lock `total_hold`/`total_wait` and `graph_busy_fraction` are
  explicitly coarse aggregates / approximations.
