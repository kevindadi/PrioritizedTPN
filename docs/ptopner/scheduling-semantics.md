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

`E_pri` keeps, within every core group, the structurally enabled transitions at
the maximal priority. Equal-priority candidates are **not collapsed**: every
admissible selected active set `E_pri-hat(M)` is a distinct scheduling branch.

A core's bound comes from `PTPN::core_parallelism` (via `parallelism_of_core`):

- A real core defaults to **1** (one active transition per core), matching the
  paper's single-server fixed-priority semantics. Every equal-priority candidate
  is a separate branch: the state space keeps all tie resolutions, because any
  of them may occur under a fixed-priority scheduler with unspecified
  tie-breaking. There is deliberately no "smallest index wins" shortcut; that
  would remove reachable schedules. A caller that models several slots registers
  a larger bound explicitly.
- The control core (`core = -1`) has no bound: it contributes one choice
  containing *all* transitions at the maximal priority of that group. It is not a
  CPU, so its transitions are not serialized by the scheduling semantics.

Ordinary control transitions all share priority `0`, so they never spuriously
suppress one another. (The `fixed_prior_with_resume` policy no longer emits any
structural resume transition; preemption and resume are handled natively by the
engine, see below.)

### Suspension

A transition is *suspended* when it is structurally enabled, filtered out by a
particular priority branch, and marked `suspendable`. Suspended transitions
freeze their execution clock (`h`) and advance a suspension clock (`w`). Control
transitions are never suspendable, so a filtered-out control transition is
simply blocked for that branch (neither active nor suspended) and becomes active
again in the next state class.

### Branch identity

The chosen active set `priority_enabled` is stored in `StateClass` and included
in the state hash and equality test, because it decides which clocks run. Two
classes can share marking, variable layout and DBM matrix but differ in the
active set; they are different semantic states and are never merged.

## Implementation

`src/analysis/reachability/scheduling.h` / `scheduling.cpp`:

```cpp
// E_struct(M)
static TransitionSet structural_enabled(const petri::PTPN& net,
                                        const petri::Marking& marking);

// Every admissible selected active set E_pri-hat(M)
static std::vector<TransitionSet> priority_branches(
    const TransitionSet& struct_enabled, const petri::PTPN& net);
```

`priority_branches`:
1. Group structurally enabled transitions by `transition.core`.
2. For each group, read its bound `K = net.parallelism_of_core(core)` and keep
   the transitions at the group's max priority (`top`).
   - `K <= 0` or `K >= |top|`: one choice, `top`.
   - `K >= 1`: every K-subset of `top` is a choice. For `K = 1` this is exactly
     the set of equal-priority tie resolutions.
3. Return the cartesian product of the per-core choices, sorted and deduplicated.

## When these sets are recomputed

`StateClassReachabilityGraph::recompute_sets` (`src/analysis/reachability/state_class_layout.cpp`)
is called whenever the marking changes (in `compute_initial_classes` and after
each `fire`). It:
1. Recomputes `struct_enabled` from the new marking (`Scheduling::structural_enabled`).
2. Installs one selected active set (`Scheduling::priority_branches`); the
   caller enumerates every admissible set and creates one successor per set.
3. Derives `suspended = { t ∈ struct_enabled \ priority_enabled | suspendable(t) }`.

`time_elapse` does NOT change these sets — it only pushes the symbolic clock zone
forward. Suspension state is recomputed only when the marking changes.

## Resume policy: engine-native preemption

The `fixed_prior_with_resume` policy (and the legacy `fixed` alias) relies on the
sets above to model preemption directly, instead of any structural encoding:

- It does NOT create the CPU-resource place. Instead it registers a parallelism
  bound of **1 for every real core** (`PTPN::core_parallelism`), so each branch
  keeps exactly one active transition per core. This enforces CPU mutual
  exclusion (one task per core, matching the physical model) and fixed-priority
  arbitration in a single step. Equal-priority contenders yield one branch per
  candidate; all branches are explored.
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

- `src/analysis/reachability/scheduling.cpp` — `structural_enabled`, `priority_branches`
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
