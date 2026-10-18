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
default), with no `--extrapolation`, no state-merging reduction
(`upper-bound`/`symmetry`), no `--horizon` pruning, and without truncation; the
other modes merge or omit state classes and the tool prints an "approximate"
warning.

`exact` describes the **state-space analysis**, not the WCET provenance. A
report can be exact while its input `time` intervals are measured or estimated
rather than certified. Inspect `wcet_kind`/`wcet_method` in `--export-wcet`, or
use `ptpn allocate` whose report includes `timing_model.all_certified` and a
per-task provenance list.

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

## Per-task cone decomposition (`--reduction cone`)

For each task, the tool projects the net onto that task's cone (its chain, the
tasks on the same real core, transitive lock holders, and dependency/periodic
control nodes) and builds a separate SCG for the projection. Only the focus
task's `TaskMetrics` is kept; the merged report sums the per-cone state counts
and recomputes the hyperperiod and analytic core utilisation from the full task
metadata.

Per-task `wcrt`, `wcrt_lb`, `wcet`, `bcet`, `deadline_missed` and
`local_deadlock` are exact (under `--canonicalization equality`, no
extrapolation, no truncation) and match the full-graph analysis. Whole-net
deadlock discovery, graph-wide lock metrics and per-place token maxima are not
available in this mode: `deadlock_states` is empty and `schedulable` reflects
only the per-task deadline/local-deadlock verdicts. Use the full graph
(`--reduction none`) when those whole-net properties are required.

Cone mode reduces work only when a task's cone is strictly smaller than the full
net. On connected task graphs (for example the bundled P-Bench/S-Bench DAGs) all
cones equal the full net, so cone mode is slower; it is an opt-in mode for
loosely coupled multi-core models, not the default.

## Task-symmetry quotient (`--reduction symmetry`)

The opt-in symmetry mode quotients the SCG by permutations of tasks that are
**proved isomorphic** on distinct real cores. Each candidate permutation must be
verified as an automorphism of the whole net (markings, arc weights, capacities,
intervals, priorities, suspension flags, periodic markers, and a consistent
relabelling of cores with equal parallelism); see the README section for the
supported scope. Every class is replaced by the lexicographically smallest
representative of its orbit, so the graph stores one vertex per symmetry class.

Consequences for the metrics:

- The exhaustive active-set semantics are unchanged: admissibility is
  canonicalized, never filtered.
- Per-task `wcrt` and `wcrt_lb` are preserved: symmetric tasks share the same
  true values, and members of a verified group are pooled to the maximum
  observed value. On correctly symmetric models these reproduce the unreduced
  per-task results.
- Secondary timing fields (`bcrt`, `jitter`, `worst_interference`,
  `worst_blocking`) are **conservative bounds**, because a quotient state
  represents several concrete schedules. `deadline_missed`, `slack`,
  `local_deadlock` and `schedulable` are evaluated from the conservative pooled
  values.
- `exact` is `false` whenever the quotient is applied. If the net admits no
  non-trivial verified group, the mode logs that fact and the graph is
  unreduced, so the normal exactness rules apply.

```bash
# 6 identical tasks on 6 distinct cores: 792 states -> 34 states
./build/ptpn tdg -f example/symmetry/symmetric-6t-6c.json \
    -m 10000000 --reduction symmetry --export-metrics metrics.json
```

## Time-horizon reduction

For WCRT-oriented runs the SCG exploration can be bounded by a wall-clock time
horizon `H`:

```bash
./build/ptpn tdg -f model.json --horizon 1000 --export-metrics metrics.json
```

`--horizon N` is disabled by default. When enabled, every vertex tracks the
**minimum accumulated lower-bound dwell** from the initial class over all paths
discovered so far (`dwell_min` is a sound lower bound on the time spent in the
source class before each firing, so the sum along a path is a lower bound on the
elapsed time of that path). Exploration is then ordered by that accumulated time
(a Dijkstra-style worklist; edge weights are non-negative). A state class whose
minimum accumulated time already exceeds `H` is still **recorded together with
its incoming edges**, but it is never expanded; no state is deleted, and a state
reached along several paths is kept with its minimum time. Without `--horizon`
the original exhaustive BFS is used unchanged.

**Soundness for WCRT.** The reduction preserves WCRT values when `H` is a valid
upper bound on the accumulated time of **every prefix of the response paths of
interest**, measured from the initial class. This is stricter than bounding the
response time itself when releases do not happen at `t = 0`: TDG lowering emits
the first periodic release at `t = period`, so a model with a periodic task of
period `P` requires approximately `H >= P + WCRT` (first-release offset plus the
response time) before its response metrics are certified. With a smaller `H`
the activation may be cut before completion and the tool reports an
approximate (possibly under-estimated) WCRT instead of an exact value.

**What is reported under pruning.** If any class was pruned:

- `exact` is `false` in the metrics report and JSON;
- the JSON gains a `horizon` object with `bound`, `truncated`, `pruned_states`
  and `deadlock_analysis_certified`;
- deadlock detection, schedulability and steady-cycle/SCC analysis are **not
  certified** and are skipped. `deadlock_analysis_certified` is `false`,
  `deadlock_states` is empty, `"schedulable"` is serialized as `null`, and
  `has_steady_cycle`/`recurrent_scc_size`/`graph_busy_fraction` are reported as
  zero/absent. (Boundary classes are sinks by construction, so running those
  analyses on the pruned graph would produce false deadlocks.) The analytic
  utilisation interval depends only on task metadata and is still reported.
- The `[SCG]` log line reports `horizon_pruned=K`, and the SCG statistics expose
  `horizon_truncated`/`horizon_pruned_states`.

Certification of timing values additionally requires
`--canonicalization equality` and no `--extrapolation`; the pipeline prints a
warning if a horizon is combined with another canonicalization mode. The
horizon only affects the state-class exploration performed for metrics/WCRT; it
does not change active-set (priority) semantics.

For benchmarking, pick `H` from a known WCRT upper bound plus the first-release
offset, then compare states, edges and WCRT against the exhaustive run. Tasks
whose complete response path lies inside `H` should reproduce the exhaustive
interval; tasks cut by the horizon may be under-reported and the whole report is
marked approximate.

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
  "horizon": { "bound": H, "truncated": false, "pruned_states": 0,
               "deadlock_analysis_certified": true },
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
is serialised as `null`. The `horizon` object is present only when `--horizon`
is enabled; `schedulable` is `null` whenever horizon pruning left the graph
incomplete (see [Time-horizon reduction](#time-horizon-reduction)).

## Soundness conventions

- Dwell bounds are per-state-class sound upper/lower bounds; they ignore
  cross-state DBM correlation (the standard SCG over-approximation), so WCRT is a
  sound upper bound, `wcrt_lb` a sound lower bound, and BCRT (after the `bcet`
  lift) a sound lower bound. The exact value lies in `[wcrt_lb, wcrt]`.
- `exact` is true only when the graph is complete
  (`--canonicalization equality`, no `--extrapolation`, no
  `--reduction upper-bound`, not truncated, and no horizon pruning). With
  extrapolation, canonicalization merging, or upper-bound zone merging, timing
  values are bounds and the flag is false.
- Under `--reduction upper-bound`, `wcrt` is a sound upper bound and
  `wcrt_lb_valid` is false. The lower bound is computed only on an exact
  (unmerged) graph.
- A `--horizon` cut is sound for WCRT only when `H` bounds the accumulated time
  of every prefix of the response path of interest (including any first-release
  offset such as `t = period`). Under pruning the report is explicitly
  non-exact, and deadlock/schedulability/SCC results are not certified
  (`deadlock_analysis_certified = false`, `schedulable = null`). See
  [Time-horizon reduction](#time-horizon-reduction).
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
