# PTPN Scheduling Semantics

## Core claim

PTPN scheduling semantics are explicit: every state class carries the
structurally enabled set (`struct_enabled`, E), the priority-filtered active set
(`priority_enabled`, X / E_pri), and the suspended set (`suspended`, R) as
first-class components. Priority filtering and suspension are operational set
operations, not encoded as implicit timed constraints.

## Formal sets

```
E_struct = { t | t is structurally enabled under marking M }
E_pri    = { t ∈ E_struct | pi(t) = max_{u ∈ E_struct, core(u)=core(t)} pi(u) }
R        = { t ∈ E_struct \ E_pri | suspendable(t) }
```

where `pi(t)` is the priority of transition `t` and `core(t)` is its core
attribute (taken straight from the JSON / `.ptpn` input).

### Per-core priority filtering (including the control core)

`E_pri` keeps, within every core group, the highest-priority structurally
enabled transitions on that core, up to the core's parallelism bound. **The
control core (`core = -1`) is treated like any other group** — control
transitions are filtered by priority too, they are no longer "always kept".

A core's bound comes from `PTPN::core_parallelism` (via `parallelism_of_core`):

- A real core under the resume policy is bounded to **1** (one task per core).
  When several transitions tie at the top priority, the filter keeps the one
  with the smallest transition index, so per-core mutual exclusion is always
  enforced.
- A core with **no** registered bound (the control core, and every core under
  the restart / PToPNer paths) is unbounded: the filter keeps *all* transitions
  at the maximal priority of that group.

Ordinary control transitions all share priority `0`, so they never spuriously
suppress one another. (The `fixed_prior_with_resume` policy no longer emits any
structural resume transition; preemption and resume are handled natively by the
engine, see below.)

### Suspension

A transition is *suspended* when it is structurally enabled, filtered out by the
priority comparison, and marked `suspendable`. Suspended transitions freeze their
execution clock (`h`) and advance a suspension clock (`w`). Control transitions
are never suspendable, so a filtered-out control transition is simply blocked for
that instant (neither active nor suspended) and becomes active again in the next
state class.

## Implementation

`src/analysis/reachability/scheduling.h` / `scheduling.cpp`:

```cpp
// E_struct(M)
static std::set<size_t> structural_enabled(const petri::PTPN& net,
                                           const petri::Marking& marking);

// E_pri(M): per-core max-priority filtering over every core group, -1 included
static std::set<size_t> filter_priority_per_core(
    const std::set<size_t>& struct_enabled, const petri::PTPN& net);
```

`filter_priority_per_core`:
1. Group structurally enabled transitions by `transition.core`.
2. For each group, read its bound `K = net.parallelism_of_core(core)`.
   - `K <= 0` (unbounded): keep every transition at the group's max priority.
   - `K >= 1`: sort by priority (descending), then transition index (ascending),
     and keep the first `K`. Real cores under the resume policy use `K = 1`.

## When these sets are recomputed

`StateClassReachabilityGraph::recompute_sets` (`src/analysis/reachability/ptpn_analysis.cpp`)
is called whenever the marking changes (in `compute_initial_class` and after each
`fire`). It:
1. Recomputes `struct_enabled` from the new marking (`Scheduling::structural_enabled`).
2. Computes `priority_enabled` via `Scheduling::filter_priority_per_core`.
3. Derives `suspended = { t ∈ struct_enabled \ priority_enabled | suspendable(t) }`.

`time_elapse` does NOT change these sets — it only pushes the symbolic clock zone
forward. Suspension state is recomputed only when the marking changes.

## Resume policy: engine-native preemption

The `fixed_prior_with_resume` policy (and the legacy `fixed` alias) relies on the
sets above to model preemption directly, instead of any structural encoding:

- It does NOT create the CPU-resource place. Instead it registers a parallelism
  bound of **1 for every real core** (`PTPN::core_parallelism`), so the priority
  filter keeps at most one active transition per core. This enforces CPU mutual
  exclusion (one task per core, matching the physical model) and fixed-priority
  arbitration in a single step. When two equal-priority transitions contend on a
  core, the filter keeps the lower transition index, a deterministic tie-break.
- It does NOT generate the preempt/suspended/resume sub-net. A preempted
  execution segment is marked `suspendable`, so it lands in the suspended set,
  freezes its `h` clock, and resumes from the frozen value once the higher-
  priority task on its core finishes (`build_successor_zone` preserves the
  surviving clock). This permits mid-segment preemption.

The `fixed_prior_with_restart` policy and the PToPNer export path keep the
structural CPU place and structural preemption sub-net unchanged. They register
no parallelism bound, so the filter falls back to "keep every highest-priority
transition" for them.

Note: multi-core parallelism per CPU (`cores_per_cpu` > 1) is intentionally not
modeled here -- the resume policy fixes the bound at one task per core. Spin-lock
"hold the CPU" behavior is only approximated by keeping spin-lock execution
segments non-suspendable.

## Key files

- `src/analysis/reachability/scheduling.cpp` — `structural_enabled`, `filter_priority_per_core`
- `src/analysis/reachability/ptpn_analysis.cpp` — `recompute_sets`, `time_elapse`, `is_firable`, `fire`, `build`, `build_successor_zone`
- `src/lower/tdg2pn/tdg2pn.cpp` — `is_resume_policy`, `add_resources_and_bindings_matrix`, `add_execution_chain`, `fixed_prior_with_restart`

## Contrast with Roméo

- Roméo keeps priority handling implicit inside timed symbolic firability checks
  in `VZone::firable`. A lower-priority transition is excluded from the symbolic
  firing set by DBM constraints.
- This repository makes the same effect explicit as set operations on the state
  class, enabling direct reasoning about which transitions are active, suspended,
  or blocked, and why.
- See `docs/romeo/priority-semantics.md` for the Roméo equivalent.

## Further reading

- Formal semantics: `unconfirmed/ptpn-formal-semantics.tex`
- State-class structure: `docs/ptopner/time-and-state-class.md`
- Roméo contrast: `docs/romeo/priority-semantics.md`
