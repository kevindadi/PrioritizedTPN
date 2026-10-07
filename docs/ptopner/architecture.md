# PTPN / PToPNer Architecture

## Core claim

This repository has two main entry points: `ptpn tdg` (JSON TDG → PTPN analysis → optional .ppn export) and `ptpn ptpn` (PTPN → analysis → export). All PTPN semantics — state classes, time, scheduling, and suspension — are implemented in `src/analysis/`. PToPNer export is a separate, constrained lowering path.

## CLI entry

`src/app/main.cpp` defines the top-level CLI via CLI11. Two subcommands are relevant:

- **`ptpn tdg <json>`** — JSON TDG input
- **`ptpn ptpn <ptpn-source>`** — PTPN source input

Both ultimately route to `run_ptpn_postprocess` (for the analysis pipeline) after optional TDG→PTPN lowering.

## TDG → PTPN lowering

```cpp
// src/app/main.cpp, build_ptpn_from_tdg()
converter::TDG2PN::transform(tdg, ptpn);
```

This is implemented in `src/lower/tdg2pn/tdg2pn.h` and `src/lower/tdg2pn/tdg2pn.cpp`. It converts a task-dependency graph (from JSON) into a PTPN. Full lowering rules are in `docs/rule.md`.

## PTPN core model

`src/model/petri.h` defines the PTPN net:

```cpp
struct Transition {
  TimeInterval time_interval;  // [earliest, latest]
  int priority;                 // higher value = higher priority
  int core;                     // -1 = control, >= 0 = core id
  bool suspendable;              // whether this transition can be suspended
};
```

Places hold tokens; a `Marking` is a `vector<int>`. Full formal semantics are in `docs/ptpn-formal-semantics.md`.

## State-class reachability analysis

```cpp
// src/app/main.cpp, run_ptpn_postprocess()
state_class::StateClassReachabilityGraph reachability_graph(ptpn);
reachability_graph.set_canonicalization_mode(canonicalization);
reachability_graph.build(opts.max_states);
```

`src/analysis/reachability/ptpn_analysis.h` defines `StateClassReachabilityGraph`, which drives timed exploration:

- `build()` — construct the reachability graph
- `time_elapse()` — push all active clocks forward (min of active upper bounds)
- `fire()` — fire a transition from a time-elapsed class, update marking and clocks
- `recompute_sets()` — recompute enabled/active/suspended after a marking change

## Scheduling and suspension

`src/analysis/reachability/scheduling.h` and `src/analysis/reachability/scheduling.cpp` define `Scheduling`:

- `structural_enabled()` — E_struct(M): transitions whose input places hold enough tokens
- `filter_priority_per_core()` — E_pri(M): per-core maximal-priority filter, bounded by the core's parallelism when declared

## PToPNer export

```cpp
// src/app/main.cpp, run_tdg_pipeline() / run_export_ptopner()
const auto ppn_validation = ptopner_export::validate_for_ptopner(tdg);
const auto ppn_export = ptopner_export::export_ptpn_to_ppn_file(ptpn, opts.ppn_file);
```

Validation and export live in `src/lower/tdg2ptopner/validate.cpp` and `src/lower/tdg2ptopner/tdg2ptopner.cpp`. The export is constrained: point intervals only, no locks, and `fixed_prior_with_restart` policy.

## Key file map

| File | Role |
|---|---|
| `src/app/main.cpp` | CLI entry, pipeline orchestration |
| `src/model/petri.h` | PTPN net model: Place, Transition, Marking, is_enabled, fire |
| `src/analysis/reachability/ptpn_analysis.h/.cpp` | StateClassReachabilityGraph: build, time_elapse, fire, recompute_sets |
| `src/analysis/reachability/state_class.h` | StateClass: marking, clocks, zone, enabled/active/suspended sets |
| `src/analysis/reachability/scheduling.h/.cpp` | Scheduling: structural_enabled, filter_priority_per_core |
| `src/analysis/metrics/metrics.h` | MetricsAnalyzer: schedulability, task/lock/core metrics |
| `src/analysis/metrics/metrics*.cpp` | Metrics implementation split by concern (core, structural, timing, locks, utilisation, JSON) |
| `src/lower/tdg2pn/tdg2pn.h/.cpp` | TDG → PTPN lowering |
| `src/lower/tdg2ptopner/validate.cpp` | PToPNer validation: point intervals, no locks, fixed_prior_with_restart |
| `src/lower/tdg2ptopner/tdg2ptopner.cpp` | PTPN → .ppn export |

## Further reading

- Full formal semantics: `docs/ptpn-formal-semantics.md`
- TDG → PTPN lowering rules: `docs/rule.md`
- Input format reference: `docs/json_format.md`
- PToPNer external tool: `tools/PToPNer/README.md`
- Contrast with Roméo timing: `docs/romeo/time-semantics.md`, `docs/romeo/priority-semantics.md`
