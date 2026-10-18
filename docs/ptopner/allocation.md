# Core-allocation exploration

`ptpn allocate` searches candidate mappings of tasks to cores and evaluates each
candidate with the same exact state-class analysis used by `ptpn tdg`. It is a
deterministic, bounded explorer: the output names the best feasible assignment
under a chosen objective, the feasible Pareto set among the evaluated
candidates, and enough search metadata to tell whether the result is exact.

## CLI

```bash
./build/ptpn allocate -f input.json -o allocation.json
./build/ptpn allocate -f input.json -o allocation.json --objective max-load
./build/ptpn allocate -f input.json -o allocation.json \
    --max-candidates 32 --candidate-max-states 50000
./build/ptpn allocate -f input.json -o allocation.json --require-certified-wcet
```

| Option | Meaning | Default |
| --- | --- | --- |
| `-f, --file` | Input TDG JSON (required) | |
| `-o, --output` | Output allocation report JSON (required) | |
| `--objective` | `max-wcrt`, `max-utilization`, or `max-load` | `max-wcrt` |
| `--max-candidates` | Hard cap on candidate analyses (1..1000000) | `64` |
| `--candidate-max-states` | State bound passed to each candidate's SCG build | `200000` |
| `--require-certified-wcet` | Only fully certified WCET provenance may win | off |
| `--policy` | Override the TDG scheduling policy before analysis | none |
| `--debug` | Show per-candidate lowering/analysis logs | off |

Exit status is `1` only for input or output errors (missing/invalid TDG, report
file that cannot be opened). A completed search with no feasible or no eligible
allocation still exits `0` and records `best: null` in the report.

The command mutates only `TaskNode.core` for the candidate copy: task order,
priorities, periods, deadlines, and every other model field are untouched. Each
candidate is lowered with `TDG2PN` after writing the chosen cores into both
`all_task` and `nodes_type`, then analyzed with canonicalization `equality`,
suspension-clock projection on, reduction `none`, no extrapolation, and no
horizon.

## Assignment domain

For each task in task-name order:

- the domain is `task_allowed_cores[name]` when present, otherwise the singleton
  `{task.core}`;
- the domain is sorted ascending and duplicate core ids are removed;
- every core must lie in `0 .. num_cpus-1`, the real core places created by
  lowering. An empty domain, a negative core, or a core outside that range makes
  the whole search stop without evaluating anything: `complete` is `false`,
  `evaluated` is `0`, and `reason` names the offending task and core.

`domain_size` is the product of the domain sizes, saturated at `SIZE_MAX` on
overflow. A domain with one element is fixed: the searching assignment always
keeps it there.

### Exhaustive strategy

When `domain_size <= max_candidates`, all assignments are enumerated in
mixed-radix lexicographic order (tasks in name order; the last task varies
fastest; cores in ascending order). The report has `strategy: "exhaustive"`,
`complete: true`, and `evaluated == domain_size`, unless the evaluation budget
stops the loop, in which case `truncated: true`.

### Heuristic strategy

When `domain_size > max_candidates`, the explorer runs a deterministic
first-improvement local search:

1. Fixed tasks are placed first and accumulate WCET load.
2. Flexible tasks are sorted by descending total WCET upper bound, ties by task
   name, and each is placed on the allowed core with the smallest current WCET
   load, ties by smallest core id (worst-fit decreasing).
3. The seed is evaluated. Then, repeatedly, each flexible task in task-name
   order is tried on each allowed core in ascending order (its current core is
   skipped). The first trial that ranks strictly better than the current
   assignment is accepted; the scan restarts. The search stops when a full scan
   finds no improvement, or when the next analysis would exceed
   `max_candidates`.

The report has `strategy: "heuristic"` and `complete: false`. `truncated` is
`true` when the budget stopped the search before a full non-improving scan;
`reason` records that the local search did not converge.

### Evaluation budget

`max_candidates` bounds the number of PTPN analyses, not the number of stored
results. Every call to the analyzer counts, including the heuristic seed and
analyses that throw; `evaluated` never exceeds `max_candidates`. Setting the
budget to zero through the API produces no evaluation and `truncated: true`
(the CLI rejects zero).

## Candidate evaluation and metrics

Each candidate is copied, lowered, and analyzed. For the resulting metrics
report:

- `states` is the total number of state classes built
  (`Statistics::total_states`); `analysis_exact` is true when the SCG build was
  not truncated by `candidate-max-states`.
- `max_wcrt` is the maximum `wcrt` over tasks with `observed == true`
  (unobserved tasks contribute nothing). `max_wcrt_infinite` is true when any
  observed task has an infinite WCRT; in that case the report's numeric
  `max_wcrt` is `null`.
- `deadline_misses` is the sorted list of observed tasks with a deadline whose
  analysis reports `deadline_missed`.
- `max_utilization` is the maximum `CoreMetrics::util_max` over real cores. It is
  the analytic sum of `WCET/period` on that core, as computed by the metrics
  layer. Cores without tasks have utilization zero.
- `max_core_load` is the maximum, over real cores, of the sum of `TaskMetrics::wcet`
  for tasks assigned to the core. This is a WCET upper-bound load, independent
  of periods; empty cores contribute zero.
- `utilization_complete` is true only when every task has a periodic binding
  with a positive period. When it is false, `max_utilization` is still reported,
  but the `max-utilization` ranking falls back to `max_core_load`.

### Feasibility

A candidate is `feasible` exactly when all of the following hold:

- `deadlock_analysis_certified` is true;
- the metrics report is `bounded`;
- the SCG build was not truncated by `candidate-max-states`;
- the metrics report was not itself truncated;
- the report is `schedulable`;
- `deadlock_states` is empty.

An exception during lowering or analysis marks that candidate infeasible with
`analysis_exact: false` instead of aborting the search. A truncated analysis is
never feasible, so the explorer does not certify an allocation on a partial
state space.

## Objectives and ranking

All objectives are minimized. The ranking is a total order:

1. feasible candidates before infeasible ones;
2. objective comparison:
   - `max-wcrt`: smaller maximum WCRT wins; an infinite maximum is worse than
     any finite value;
   - `max-utilization`: smaller maximum utilization wins, compared with a
     tolerance of `1e-12`; when either candidate lacks complete utilization
     data, both fall back to `max_core_load`;
   - `max-load`: smaller `max_core_load` wins;
3. smaller `max_core_load`;
4. smaller `max_wcrt` (infinity ordered last);
5. lexicographically smallest `(task, core)` assignment in task-name order.

`best` is the highest-ranked feasible candidate that also passes the WCET gate.
`assignment` repeats that candidate's assignment at the top level, or is an
empty array when `best` is null.

The Pareto set contains feasible evaluated candidates that are not dominated on
`(max_wcrt, max_utilization, max_core_load)`: a candidate dominates another when
it is no worse on all three and strictly better on at least one (infinite WCRT
is treated as +infinity; utilization is compared exactly). The set is sorted by
`max_wcrt`, then `max_core_load`, then assignment, and capped at 32 entries; a
cap is reported as `pareto_truncated: true`. Infeasible candidates never enter
the Pareto set. Under the heuristic strategy the Pareto set covers only the
evaluated candidates, not the full assignment domain.

## WCET provenance gate

Every task carries a WCET provenance kind (`model`, `measured`, `estimated`, or
`certified`); a task without an explicit record is treated as `model`.
`timing_model.all_certified` is true only when every task's kind is `certified`.

With `--require-certified-wcet`, `best` is chosen only among feasible
candidates when `all_certified` is true; otherwise `best` remains null even if
feasible allocations were found, and `reason` explains the gate. Pareto entries
are unaffected by the gate: they describe the analyzed feasibility/performance
set, while certification is a property of the timing model shared by every
candidate.

## JSON report schema

Object keys are emitted in sorted order and arrays are deterministic:

```json
{
  "assignment": [{"task": "A", "core": 1}],
  "best": {
    "analysis_exact": true,
    "assignment": [{"task": "A", "core": 1}],
    "deadline_misses": [],
    "deadlock_states": 0,
    "feasible": true,
    "max_core_load": 5,
    "max_utilization": 0.5,
    "max_wcrt": 5,
    "max_wcrt_infinite": false,
    "states": 12,
    "utilization_complete": true
  },
  "pareto": [
    {
      "analysis_exact": true,
      "assignment": [{"task": "A", "core": 1}],
      "deadline_misses": [],
      "deadlock_states": 0,
      "feasible": true,
      "max_core_load": 5,
      "max_utilization": 0.5,
      "max_wcrt": 5,
      "max_wcrt_infinite": false,
      "states": 12,
      "utilization_complete": true
    }
  ],
  "search": {
    "complete": true,
    "domain_size": 2,
    "evaluated": 2,
    "feasible_count": 2,
    "objective": "max-wcrt",
    "pareto_truncated": false,
    "reason": "",
    "strategy": "exhaustive",
    "task_count": 2,
    "truncated": false,
    "utilization_complete": true
  },
  "timing_model": {
    "all_certified": false,
    "kinds": [{"task": "A", "kind": "model"}]
  }
}
```

- `search.complete`: true only when the exhaustive strategy evaluated the entire
  assignment domain. It says nothing about whether each candidate's SCG was
  untruncated (check `analysis_exact`), nor whether the WCET gate accepted the
  winner.
- `search.truncated`: the evaluation budget stopped the search before it
  finished (exhaustive or heuristic).
- `search.domain_size`: saturated product of the domains.
- `search.reason`: empty when the search was complete, untruncated, feasible,
  and eligible; otherwise a semicolon-separated explanation.
- `best` and each `pareto` entry use the same candidate object schema shown
  above. `best` is `null` when no feasible, gate-eligible candidate was found.

## Assumptions and limits

- Real cores are `0 .. num_cpus-1`; the TDG validator enforces this range.
  `cores_per_cpu` is a per-core capacity parameter and does not create
  additional allocation targets.
- Domains are normalized (sorted, deduplicated), so the enumeration order and
  `domain_size` are independent of the order written in `allowed_cores`.
- There is no utilization or schedulability prefilter: every candidate in the
  budget is lowered and analyzed exactly. This keeps the search independent of
  analytic heuristics, at the cost of running one SCG per candidate.
- `max_core_load` sums `TaskMetrics::wcet`, the task's upper-bound execution
  time; it is not a measured load and ignores jitter.
- The exhaustive search is optimal in the explored domain only when every
  candidate analysis is exact and utilization is complete for the chosen
  objective. The heuristic result is a deterministic local optimum, not a proof
  of global optimality.
