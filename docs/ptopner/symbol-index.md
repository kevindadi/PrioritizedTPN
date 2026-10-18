# PTPN / PToPNer Symbol Index

## Core PTPN model

| Symbol | File | Notes |
|---|---|---|
| `Transition.time_interval` | `src/model/petri.h:69` | `[earliest, latest]` time bounds |
| `Transition.priority` | `src/model/petri.h:70` | higher value = higher priority |
| `Transition.core` | `src/model/petri.h:71` | -1 = control, ≥0 = core id |
| `Transition.suspendable` | `src/model/petri.h:72` | whether the transition can be suspended |
| `kControlTransitionCore` | `src/model/petri.h:21` | constant -1, marks control transitions |
| `Marking` | `src/model/petri.h:85` | `vector<int>` of token counts per place |

## State-class representation

| Symbol | File | Notes |
|---|---|---|
| `StateClass.marking` | `src/analysis/state.h:53` | current token distribution |
| `StateClass.clocks` | `src/analysis/state.h:54` | `TransitionClock` array, auxiliary/trace view |
| `StateClass.zone` | `src/analysis/state.h:55` | DBM-backed main time representation |
| `StateClass.enabled` | `src/analysis/state.h:59` | raw enabled transitions |
| `StateClass.active` | `src/analysis/state.h:60` | enabled and not suspended |
| `StateClass.suspended` | `src/analysis/state.h:61` | enabled but clock-frozen |
| `StateClass.cumulative_time` | `src/analysis/state.h:63` | accumulated elapsed time (not part of state identity) |

## Reachability graph

| Symbol | File | Notes |
|---|---|---|
| `StateClassReachabilityGraph.build` | `src/analysis/graph.h:70` | constructs the reachability graph up to max_states |
| `StateClassReachabilityGraph.advance_time` | `src/analysis/graph.h:96` | pushes active clocks forward by min active upper bound |
| `StateClassReachabilityGraph.fire_with_time` | `src/analysis/graph.h:107` | fires transition t at specific time, returns new state |
| `StateClassReachabilityGraph.recompute_enabled_sets` | `src/analysis/graph.h:118` | recomputes enabled/active/suspended after marking change |

## Scheduling

| Symbol | File | Notes |
|---|---|---|
| `SchedulingAlgorithms::select_active_per_core` | `src/analysis/reachability/scheduling.h:26` | per-core max-priority filter |
| `SchedulingAlgorithms::select_one_transition` | `src/analysis/reachability/scheduling.h:35` | picks highest priority from schedulable set |
| `SchedulingAlgorithms::compute_suspended` | `src/analysis/reachability/scheduling.h:49` | determines which enabled non-active transitions should suspend |
| `SchedulingAlgorithms::should_suspend` | `src/analysis/reachability/scheduling.h:67` | per-transition suspend judgment |
| `SchedulingAlgorithms::should_restore` | `src/analysis/reachability/scheduling.h:83` | per-transition resume judgment |

## PToPNer export

| Symbol | File | Notes |
|---|---|---|
| `validate_for_ptopner` | `src/lower/tdg2ptopner/validate.cpp:187` | top-level validation entry |
| `validate_point_intervals` | `src/lower/tdg2ptopner/validate.cpp:22` | rejects non-point time intervals |
| `validate_no_locks` | `src/lower/tdg2ptopner/validate.cpp:48` | rejects TDG with lock modeling |
| `export_ptpn_to_ppn_file` | `src/lower/tdg2ptopner/tdg2ptopner.cpp` | actual .ppn file generation |

## TDG lowering

| Symbol | File | Notes |
|---|---|---|
| `converter::TDG2PN::transform` | `src/lower/tdg2pn/tdg2pn.h` | TDG → PTPN lowering entry |

## Where E, X, R live in code

- **E (enabled)** → `StateClass.enabled` in `src/analysis/state.h:59`
- **X (active / schedulable)** → `StateClass.active` in `src/analysis/state.h:60`, after `SchedulingAlgorithms::select_active_per_core`
- **R (suspended)** → `StateClass.suspended` in `src/analysis/state.h:61`, after `SchedulingAlgorithms::compute_suspended`

The formal state is `S = (M, C, E, X, R, Θ)` where M = marking, C = clock zone (DBM), E/X/R as above, Θ = cumulative time. See `docs/ptpn-formal-semantics.md` for the full formal definition.

## Contrast with Roméo

- Roméo stores priority on `Transition` and filters it dynamically inside `VZone::firable` as DBM constraints.
- This repository surfaces E/X/R as explicit set fields in `StateClass`, making the scheduler-facing structure first-class.
- See `docs/romeo/symbol-index.md` for Roméo's equivalents.
