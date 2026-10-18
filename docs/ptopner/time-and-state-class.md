# PTPN Time and State-Class Semantics

## Core claim

This repository's state class is explicit and scheduler-facing: a state is not just a marking plus an anonymous zone. It carries the marking, the DBM-backed time representation, the transition↔clock mapping, and the three scheduler sets `E` / `X` / `R` as first-class fields. Time advances only for `active` transitions; `suspended` transitions keep their clocks frozen.

## State-class structure

`src/analysis/state.h:52` defines the concrete state:

```cpp
struct StateClass {
  std::vector<int> marking;
  std::vector<TransitionClock> clocks;
  DBM zone;
  std::vector<int> transition_to_clock;
  std::vector<size_t> clock_to_transition;

  std::set<size_t> enabled;
  std::set<size_t> active;
  std::set<size_t> suspended;

  double cumulative_time;
  size_t state_id;
};
```

This is the implementation form of the formal state `S = (M, C, E, X, R, Θ)` in `docs/ptpn-formal-semantics.md`:

- `M` → `marking`
- `C` → `zone` as the main timed semantics, with `clocks` kept as an auxiliary / compatibility view
- `E` → `enabled`
- `X` → `active`
- `R` → `suspended`
- `Θ` → `cumulative_time`

`cumulative_time` is explicitly auxiliary metadata, not part of state identity.

## How time is represented

There are two aligned views of time:

1. `DBM zone` in `src/analysis/state.h:55` is the main symbolic time representation.
2. `TransitionClock` in `src/analysis/dbm/clock_state.h:16` is the per-transition view:

```cpp
struct TransitionClock {
  int lower_bound;
  int upper_bound;
  ClockState state;
};
```

`ClockState` (`src/analysis/dbm/clock_state.h:10`) has exactly three cases:

- `UNACTIVE` — transition not currently ticking
- `ACTIVE` — clock advances with time
- `SUSPENDED` — clock is frozen

The DBM implementation in `src/analysis/dbm/dbm.h` exposes the operations this semantics needs: `elapse_time`, `reset_clock`, `freeze_clock`, `unfreeze_clock`, `restrict_clock`, `restrict_for_firing`, `intersection`, and `is_empty`.

## Where time advances

`src/analysis/graph.h:80` documents `StateClassReachabilityGraph::advance_time` as the time-elapse step.

Its contract is:

1. Find the minimum upper bound among `active` transitions.
2. Advance all `active` clocks by that amount.
3. Keep `suspended` clocks frozen.
4. Add the elapsed amount to `cumulative_time`.
5. Return the elapsed amount.

This matches the formal rule in `docs/ptpn-formal-semantics.md`: suspended transitions do not participate in the `τ_min` computation and do not progress while frozen.

## Where firing happens

`src/analysis/graph.h:98` defines `fire_with_time`:

```cpp
std::tuple<bool, StateClass, double> fire_with_time(
    size_t t, const StateClass& from) const;
```

Its role is:

1. Check whether transition `t` is time-feasible in the current state.
2. Compute the firing time.
3. Produce the successor marking and timed state.
4. Recompute `enabled`, `active`, and `suspended` for the new marking.

So the split is:

- `advance_time()` handles pure time elapse.
- `fire_with_time()` handles one symbolic firing step.
- `recompute_enabled_sets()` rebuilds the scheduler-facing sets after the marking changes.

## Why `E / X / R` live inside the state

This project does not hide all scheduling consequences inside anonymous timing constraints. It stores scheduler-facing structure directly in `StateClass` because suspension is semantically observable here:

- `enabled` means structurally enabled under the marking
- `active` means enabled and currently ticking
- `suspended` means enabled but frozen by higher-priority competition on the same core

That is why state identity in `StateKey` (`src/analysis/state.h:107`) includes `enabled`, `active`, `suspended`, and DBM frozen-clock information, not just the marking and a raw matrix.

## Key files

- `src/analysis/state.h:52` — concrete `StateClass` layout
- `src/analysis/state.h:107` — `StateKey` fields used for equivalence / deduplication
- `src/analysis/dbm/clock_state.h:10` — `ClockState::{UNACTIVE, ACTIVE, SUSPENDED}`
- `src/analysis/dbm/dbm.h:16` — DBM operations for elapse / freeze / firing restriction
- `src/analysis/graph.h:80` — `advance_time`
- `src/analysis/graph.h:98` — `fire_with_time`
- `src/analysis/graph.h:110` — `recompute_enabled_sets`

## Contrast with Roméo

- Roméo centers timed semantics on symbolic zone transformation and timed firability checks.
- This repository also uses a symbolic timed domain, but it exposes scheduler consequences as explicit state fields.
- Practical effect: here you can directly ask "which transitions are enabled, active, or suspended in this state?" without reconstructing that from implicit priority constraints.

## Further reading

- Scheduling details: `docs/ptopner/scheduling-semantics.md`
- Symbol lookup: `docs/ptopner/symbol-index.md`
- Formal rules: `docs/ptpn-formal-semantics.md`
- Roméo contrast: `docs/romeo/time-semantics.md`
