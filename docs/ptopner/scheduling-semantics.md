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
A        = { t ∈ E_struct | admissible(t) }                 -- firing rule
E_pri    = { t ∈ A | pi(t) = max_{u ∈ A, core(u)=core(t)} pi(u) }
R        = { t ∈ E_struct \ E_pri | suspendable(t) }
```

where `pi(t) = rule.effective_priority(context, t)` is supplied by the active
`FiringRule`, and `core(t)` is the transition's core attribute (taken straight
from the JSON / `.ptpn` input). `admissible(t)` is the rule's optional
constraint; the default static rule admits every structurally enabled
transition.

## Firing-rule template

`src/analysis/reachability/scheduling_policy.h` defines the policy interface:

```cpp
enum class PreemptionMode { RESUME, RESTART, NON_PREEMPTIVE };

struct PolicyContext {
  const petri::PTPN& net;
  // Marking is always set; the full clock layout/zone may be absent when
  // priority branches are computed. Static rules must not read it.
  const StateClass* state = nullptr;
};

struct BranchConstraint {  // r[right] - r[left] <= bound
  size_t left_task;
  size_t right_task;
  int bound;
};
struct PolicyBranch {
  TransitionSet active;
  std::vector<BranchConstraint> constraints;
};

class FiringRule {
 public:
  virtual int effective_priority(const PolicyContext&, size_t transition) const = 0;
  virtual bool admissible(const PolicyContext&, size_t transition) const;  // default true
  virtual PreemptionMode preemption_mode() const = 0;
  virtual bool uses_release_clocks() const { return false; }
  virtual std::vector<PolicyBranch> branches(const TransitionSet&, const petri::PTPN&,
                                             const PolicyContext&) const;
};
```

`StaticPriorityRule` reads `TransitionId{t}.priority` from the net, admits every
transition, and carries the policy's preemption mode. Its default `branches()`
wraps `Scheduling::priority_branches` with empty constraints, so the
static/RM/DM/PIP/PCP/SRP behavior is unchanged.
`make_firing_rule(policy)` installs it by default, and
`StateClassReachabilityGraph` constructs its rule from
`net.schedule_policy()` (a directly parsed `.ptpn` defaults to `fixed`).

Current coverage:

| Policy | Priorities | Preemption mode | Analysis status |
| ------ | ---------- | --------------- | --------------- |
| `fixed` | static (`priority` from input) | `RESUME` | analyzed |
| `fixed_prior_with_resume` | static | `RESUME` | analyzed |
| `fixed_prior_with_restart` | static | `RESTART` | analyzed (structural preemption sub-net) |
| `rm` | computed from periods, see below | `RESUME` | analyzed |
| `dm` | computed from deadlines/periods, see below | `RESUME` | analyzed |
| `pip` | base `priority` + dynamic inheritance from the marking, see below | `RESUME` | analyzed |
| `pcp` | base `priority` + static lock ceilings + dynamic admission/inheritance, see below | `RESUME` | analyzed |
| `srp` | base `priority` as preemption level + dynamic global system-ceiling start gate, see below | `RESUME` | analyzed |
| `edf` | absolute deadlines from per-task release clocks, see below | `RESUME` | analyzed for the restricted single-core class |
| `llf`, `fifo` | static | `NON_PREEMPTIVE` | **not implemented**; only the static rule applies |

LLF/FIFO are accepted by the parser but must not be described as analyzed:
they currently fall back to their static transition priorities and
non-preemptive handling. EDF is not a static-priority policy; the supported
class and its restrictions are documented in the EDF section below.

### Priority Inheritance Protocol (`pip`)

`PipFiringRule` (`src/analysis/reachability/scheduling_policy.{h,cpp}`)
implements the Priority Inheritance Protocol on top of the marking. Lock
ownership is derived from the chain position of each task's single job token,
not from hold-place occupancy:

1. Every task starts with its base priority from `TaskInfo::priority` (the
   input `priority` field; it is not recomputed at lowering time).
2. A task chain carries one job token. The rule counts tokens over *every*
   place of the chain, including the lock-hold places, and records the chain
   position `p` of the single token.
3. Chain layout: lock `i` is acquired by the transition at position
   `5 + 4*i` and released by the transition at
   `chain.size() - 4 - 2*i` (mirroring
   `TaskChainLayout::lock_release_transition`). The task holds lock `i`
   exactly when `acquire_position + 1 <= p <= release_position - 1` and the
   shared lock place is empty. The range starts at the hold place and runs
   through the release transition's pre-place, so it covers the complete
   critical section. In a multi-lock chain the token may leave the hold place
   several segments before the release transition fires; the holder is still
   detected. A non-empty lock place contradicts the range claim; inheritance
   through that lock is then skipped.
4. A task is *waiting* for lock `i` when `p == acquire_position - 1` and the
   acquire transition is not structurally enabled. This test uses the marking,
   not only `struct_enabled`: a blocked acquire is absent from the structurally
   enabled set, and that is exactly the situation in which inheritance is
   required.
5. The rule maps each lock to its unique holder and propagates
   `eff[holder] = max(eff[holder], eff[waiter])` to a fixpoint (at most one
   round per task; a wait cycle simply saturates at the maximum base priority).
   This transitive closure handles nested critical sections: if H waits for a
   lock held by M and M waits for a lock held by L, both M and L inherit H's
   priority.
6. The effective priority is per task and therefore applies to every transition
   in the task's chain. Control and non-task transitions keep their statically
   encoded priority (ordinary control transitions use 0).

PIP changes only priorities: `admissible()` is always true and the preemption
mode is `RESUME`. Lowering treats `pip` like the other resume policies: no
structural `coreN` place is created and execution segments are marked
suspendable, so preemption is expressed by the engine's per-core priority
filter and execution-clock freeze.

Supported class and defensive fallbacks:

- **Capacity.** PIP is supported for TDG2PN-lowered nets with
  `task_place_capacity == 1`. Lowering rejects larger capacities with a clear
  `std::invalid_argument`, because queued job tokens would make the chain
  position ambiguous. It also rejects start bindings requesting more than one
  token for a task. Hand-written `.ptpn` nets default to `FIXED`, so PIP is
  only reachable through lowering.
- **Exactly one token per chain.** The position-range rule is exact only when
  every task chain contains exactly one job token. If a marking has zero or
  multiple tokens in a chain (for example a queued periodic release), that
  task is unsupported for the state: it keeps its base priority and
  contributes neither a wait edge nor a holder claim. This is a defensive
  fallback; no WCRT guarantee is claimed for such states.
- **One claimant per lock.** If two tasks claim the same lock, or the shared
  lock place is non-empty while a range claim exists, the marking is
  inconsistent and inheritance through that lock is disabled. This is also a
  defensive fallback, not a proven analysis result.
- **Layout.** Lock discovery relies on `TaskInfo::locks` and the TDG2PN chain
  layout. A chain that does not match the layout has no recognized
  acquire/release positions, which degrades to base priorities.
- PToPNer export validation still accepts only `fixed_prior_with_restart`;
  `pip` remains rejected for that backend.

### Priority Ceiling Protocol (`pcp`)

`PcpFiringRule` (`src/analysis/reachability/scheduling_policy.{h,cpp}`)
implements PCP over the same chain-position lock topology as PIP
(`detail::TaskLockTopology`). Base priorities are the input
`TaskInfo::priority`; **PCP does not combine with the RM/DM priority
computation in this phase** (a `pcp` run uses the input priorities unchanged).

1. **Lock ceilings.** `ceiling(L)` is the maximum base priority over every task
   whose `TaskInfo::locks` contains `L`. A lock no task uses has no holder and
   no ceiling. Ceilings are static and independent of the current marking.
2. **Held locks.** Ownership uses the same range rule as PIP: a task holds
   lock `i` when its single job token lies in
   `[acquire_position + 1, release_position - 1]` and the shared lock place is
   empty. No hold-place occupancy is assumed.
3. **System ceiling.** For task `J`, the system ceiling is the maximum ceiling
   over locks currently held by tasks other than `J`. `J`'s own locks are
   excluded. If no other task holds a lock, the system ceiling is empty and
   admission is always allowed; there is no `0` sentinel.
4. **Admission (ceiling rule).** `J` may fire the acquire transition for a
   free lock `L` only when `base_priority(J) > system_ceiling(J)` (strict).
   A lock held by another task is already structurally disabled; that is a
   direct block, not an admission decision, so `admissible()` does not reject
   it. Ceiling blocking is applied by `admissible()` before the per-core
   priority filter.
5. **Direct blocking.** When `J` requests a lock held by `H`, the wait edge
   `J -> H` is added.
6. **Ceiling blocking.** When `J` requests a *free* lock and
   `base_priority(J) <= system_ceiling(J)`, `J` is blocked by the holder of a
   held lock attaining the system ceiling. If several held locks share the
   maximum ceiling, the lexicographically smallest lock name is chosen (the
   holder map is iterated in `std::map` order) and its unique holder receives
   the wait edge `J -> holder`. This defines a deterministic PCP instance; it
   is not an arbitrary tie.
7. **Inheritance.** Effective priorities are the least fixpoint of
   `eff[holder] = max(eff[holder], eff[waiter])` over direct and ceiling wait
   edges, in at most `task_count` rounds. A wait cycle saturates at the maximum
   effective priority. A task's effective priority applies to every transition
   in its chain; control/non-task transitions keep their static priority.
8. **Nested locks.** Nesting is permitted. Because the system ceiling excludes
   `J`'s own held locks, `J` may acquire another free lock exactly when its
   base priority exceeds the ceiling of locks held by *other* tasks.
9. **Preemption.** PCP uses the `RESUME` encoding, like `fixed` and PIP: no
   structural `coreN` place, `core_parallelism = 1` per core, and suspendable
   execution segments.
10. **Multi-core scope.** The system ceiling is global across the whole net: a
    lock held on one core can block a lock acquisition or raise a holder's
    priority on another core. Classical PCP is a uniprocessor protocol; on
    several cores this is the global-ceiling PCP variant, not MSRP/OMLP, and it
    does not claim multiprocessor blocking bounds.

Supported class and defensive fallbacks:

- **Capacity and job count.** TDG2PN lowering rejects
  `task_place_capacity != 1` and start bindings requesting more than one token
  for `pcp`, exactly as for `pip`. The position-range rule is exact only when
  each task chain contains exactly one job token; one start token per binding
  preserves that for start tasks.
- **Unsupported or inconsistent markings.** If a chain has zero or multiple
  tokens, or a lock place is non-empty while a range claim exists, or several
  tasks claim one lock, the affected task/lock contributes no holder claim and
  no inheritance. This is a defensive fallback: no WCRT upper-bound guarantee
  is claimed for such states, unlike the supported class.
- Hand-written `.ptpn` nets default to `FIXED`, so `pcp` is only reachable
  through lowering.
- PToPNer export validation still accepts only `fixed_prior_with_restart`;
  `pcp` is rejected for that backend together with `pip`.

### Stack Resource Policy (`srp`)

`SrpFiringRule` (`src/analysis/reachability/scheduling_policy.{h,cpp}`)
implements the Stack Resource Policy over the same chain-position lock topology
as PIP/PCP (`detail::TaskLockTopology`). Base priorities are the input
`TaskInfo::priority`; `srp` does not combine with the RM/DM priority
computation.

1. **Preemption level.** The preemption level of a task is its base priority,
   i.e. the input `priority` field. The same value is used as the scheduling
   priority. It is not recomputed at lowering time. Assigning preemption
   levels/deadlines consistently (in the classical SRP sense) is the caller's
   responsibility; the analysis does not validate or derive this relation.
2. **Lock ceilings.** `ceiling(L)` is the maximum base priority over every task
   whose `TaskInfo::locks` contains `L`. A lock no task uses has no ceiling.
   Ceilings are static and independent of the current marking.
3. **Global system ceiling.** At a marking, the system ceiling is the maximum
   ceiling over *all* locks currently held by any task, including locks held by
   the candidate task itself. If no lock is held, the system ceiling is empty
   and admission is always allowed; there is no `0` sentinel. Held-lock
   ownership uses the PIP/PCP range rule (single job token in
   `[acquire_position + 1, release_position - 1]` with an empty shared lock
   place).
4. **Start gate.** Only the `get_core` transition of a task chain (position 1)
   is constrained. It is admissible only when the task's job token is at the
   chain entry (position 0) *and* either no lock is held anywhere in the net or
   `base_priority(task) > system_ceiling` (strict). Otherwise the task remains
   at the entry place and cannot begin executing. Every other transition is
   admitted by `admissible()`; in particular there is no PCP-style admission
   test at lock-acquire transitions (the stack discipline makes it redundant).
5. **No re-check while running.** Once a task has fired `get_core`, its token
   leaves the entry place and never returns during that job, so the start gate
   no longer applies. A started task is never blocked again by a later rise of
   the system ceiling (for example when a nested lock is acquired).
6. **No priority inheritance.** `effective_priority()` returns the transition's
   statically encoded priority. The system ceiling affects only admission; it
   does not raise the holder's scheduling priority.
7. **Preemption.** SRP uses the `RESUME` encoding, like `fixed`, PIP and PCP:
   no structural `coreN` place, `core_parallelism = 1` per real core, and
   suspendable execution segments.
8. **Nested locks and deadlock avoidance.** Nesting is permitted. The system
   ceiling includes all currently held locks, and acquisitions beyond the start
   gate are not admission-tested. The stack discipline follows from the start
   gate: a task can start only when its preemption level exceeds the ceiling of
   every held lock, so it cannot begin by blocking on a task that is already
   inside a critical section (under consistent preemption levels/ceilings).
   This prevents the hold-and-wait cycles that PCP handles with its
   acquire-time ceiling rule. The start gate alone is the SRP admission
   mechanism; there is no acquire-time check.
9. **Multi-core scope.** The system ceiling is global across the whole net: a
   lock held on one core gates starts on every core. This is the
   global-ceiling variant of SRP, not the multiprocessor MSRP variant.
   Classical SRP is defined for uniprocessor systems; on several cores this
   encoding is conservative (it may serialize independent cores while any
   sufficiently high-ceiling lock is held), and it does not claim optimal
   multi-core blocking bounds.

Supported class and defensive fallbacks:

- **Capacity and job count.** TDG2PN lowering rejects
  `task_place_capacity != 1` and start bindings requesting more than one token
  for `srp`, exactly as for `pip`/`pcp`. The start gate and the held-lock
  topology are exact only when each task chain contains exactly one job token;
  one start token per binding preserves that for start tasks.
- **Unsupported or ambiguous markings.** If a chain has zero or multiple
  tokens, or the marking size does not match the net layout, the affected task
  is unsupported for the state and the start gate is skipped (the transition
  is admitted). This is a defensive fallback: such states are not certified
  SRP-exact and no WCRT upper-bound guarantee is claimed for them.
- Hand-written `.ptpn` nets default to `FIXED`, so `srp` is only reachable
  through lowering.
- PToPNer export validation still accepts only `fixed_prior_with_restart`;
  `srp` is rejected for that backend together with `pip`/`pcp`.

### Per-core priority filtering (including the control core)

`E_pri` keeps, within every core group, the admissible transitions at the
maximal effective priority. Equal-priority candidates are **not collapsed**:
every admissible selected active set `E_pri-hat(M)` is a distinct scheduling
branch.

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
suppress one another. (The resume policies no longer emit any structural resume
transition; preemption and resume are handled natively by the engine, see
below.)

### Suspension

A transition is *suspended* when it is structurally enabled, filtered out by a
particular priority branch, and marked `suspendable`. Suspended transitions
freeze their execution clock (`h`) and advance a suspension clock (`w`). Control
transitions are never suspendable, so a filtered-out control transition is
simply blocked for that branch (neither active nor suspended) and becomes active
again in the next state class. The analysis projects `w` out of the DBM by
default (`--keep-suspension-clocks` retains it); the suspended set and the
frozen execution clocks are unchanged, so this is an exact optimization.

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
    const TransitionSet& struct_enabled, const petri::PTPN& net,
    const FiringRule& rule, const PolicyContext& context);
```

`priority_branches`:
1. Filter `struct_enabled` through `rule.admissible(context, t)`.
2. Group the admissible transitions by `transition.core` and sort each group by
   `rule.effective_priority(context, t)` descending; transition id is used only
   as a deterministic enumeration tie-break.
3. For each group, read its bound `K = net.parallelism_of_core(core)`.
   - `K <= 0` (control core): one choice, all transitions at the group's max
     effective priority.
   - Enough slots: one choice containing every admissible transition.
   - `K >= 1`: the transitions strictly above the cutoff priority are
     mandatory; every K-subset completion from the ties at the cutoff is a
     choice. For `K = 1` this is exactly the set of equal-priority tie
     resolutions.
   - A group with no admissible transition contributes no choice.
4. Return the cartesian product of the per-core choices, sorted and
   deduplicated. If every group is empty (no admissible transition), the result
   is one empty active set.

### Where the context comes from

- `compute_initial_classes` builds the context over the initial `base` state
  (marking set, zone/layout not yet built).
- `fire_all` builds a temporary `StateClass` carrying the successor marking.
  The full successor zone/layout is only available after the branch is chosen.
  Static rules never read `PolicyContext::state`. Dynamic rules such as
  `PipFiringRule`, `PcpFiringRule` and `SrpFiringRule` read only the marking,
  which is already present; they must not depend on the zone or clock layout at
  this point.

## RM and DM

RM and DM are analyzed as fixed-priority policies whose priorities are computed
during TDG lowering (`src/lower/tdg2pn/policy_priorities.*`):

- **RM key** = the periodic period (`configuration.periodic[].period`).
  Ascending key = higher priority (shorter period wins). Tasks without a
  positive period get priority `0`.
- **DM key** = the explicit task `deadline` when `> 0`, otherwise the periodic
  period. Ascending key = higher priority (shorter deadline wins). Tasks with
  neither get priority `0`.
- Equal keys share one integer priority. Distinct key groups are ranked so the
  highest-priority group gets the largest integer and the lowest group gets
  `1`; tasks without a key get `0`. Task names only make rank assignment
  deterministic; they do not split a tie group.
- The computed ranks replace `TaskNode.priority` in `all_task` and
  `nodes_type`, and populate `tasks_priority`. **Explicit `priority` values in
  the input are ignored for RM/DM.**
- `TDG2PN::transform` applies this on a local copy of the TDG, so the caller's
  TDG is never mutated, and records the policy on the PTPN
  (`PTPN::schedule_policy`).
- RM/DM use the same resume-style preemption and resource lowering as
  `fixed`/`fixed_prior_with_resume`: no `coreN` resource places, one task per
  core via `core_parallelism`, and suspendable execution segments. The
  state-class engine then applies the computed static priorities through
  `StaticPriorityRule`.

## EDF (restricted supported class)

EDF is implemented by `EdfFiringRule` and is **not** a static deadline
priority. Relative deadlines from `TaskInfo` are not enough: EDF orders jobs by
their absolute deadlines `r_i + D_i`, which depend on when each job was
released. The analysis therefore represents a per-job release clock in the
state and splits each symbolic zone by deadline order.

### Supported class

The supported class is deliberately narrow. TDG lowering
(`validate_edf_supported_class` in `src/lower/tdg2pn/tdg2pn.cpp`) rejects the
whole model when any of these does not hold, and `EdfFiringRule::ensure_index`
re-checks the lowered net:

- `num_cpus == 1`, every task bound to core `0`.
- No locks (`shared_locks` / task `locks` must be empty), no fork/join
  (every real-core transition belongs to exactly one task chain).
- `task_place_capacity == 1`; all task places saturate.
- At most one start token per task (`configuration.start[].tokens <= 1`).
- Every task has a positive absolute deadline (`deadline`, or an implicit
  deadline when only a period is given) and is either periodic with a positive
  period released by `_fire`, or a one-shot task started once at time 0.
- No task-to-task TDG edge (dependency-driven release). Self-loops and dashed
  periodic-release edges are not dependency releases.
- The standard TDG2PN no-lock task chain: alternating place/transition,
  `entry/get_core/ready/first-exec/.../exit`, at least 5 nodes.
- No periodic release may fire while the same task's job is between core
  acquisition and completion (see "Periodic releases" below).

Unsupported models raise `std::invalid_argument` during lowering or analysis;
they are never approximated by a static deadline priority.
`EdfFiringRule::effective_priority` raises `std::logic_error`, so no code path
can silently use a scalar priority for EDF.

### State extension: release clocks

`ClockKind::Release` extends the DBM layout with one clock `r_i` per task:

- `r_i` is created when task `i` is released (periodic `_fire` or the initial
  start token) and is removed when the final execution segment places the
  completion token in the exit place. A pending exit token (before the
  end-of-task consumer fires) is a completed job and carries no release clock.
- `r_i` starts at zero at release and always advances at rate 1. It keeps
  advancing while the job is preempted; only the execution clock of a
  suspended segment freezes. This keeps the remaining deadline `D_i - r_i`
  comparable across a preemption.
- The clock layout is `x0`, then release clocks of in-flight tasks in ascending
  task ordinal order, then execution clocks. Release-clock indices therefore do
  not depend on the selected active set.
- `StateClass::release_clock_of_task` maps task ordinal to DBM column (or `-1`)
  and is part of the state identity (hashed and compared with the clock
  layout). No release information lives in non-hashed metadata.
- `r_i` and the deadline constraints are part of the zone. The initial class
  pins every clock to zero. A successor keeps `r_i` for a job that remains in
  flight, drops it on completion, and pins a freshly released job's clock to
  zero.

### Deadline-order branches

At each state, `EdfFiringRule::branches` collects the structurally enabled
chain transitions per in-flight task. A task `i` can be the earliest only if
the current zone admits

```
D_i - r_i <= D_j - r_j  for every other in-flight task j
<=>  r_j - r_i <= D_j - D_i
```

For each feasible candidate it emits one `PolicyBranch` whose active set is
all control (`core < 0`) transitions plus that task's chain transitions, and
whose `BranchConstraint{candidate, j, D_j - D_candidate}` list records the
order. Equal remaining deadlines make multiple candidates feasible; every tie
resolution is emitted. At capacity one there is no additional cutoff subset,
so these are exactly the EDF choices.

The engine intersects the constraints with the successor zone (using
`DBM::tighten`) **before** hashing/deduplication. This is the key difference
from a static deadline rank: one symbolic state may still contain several
possible deadline orders, and the graph splits it instead of fixing an order
inside a shared DBM. A branch whose ordering is impossible in the successor
zone is dropped. If no candidate is feasible, the analysis raises an error
rather than emitting a partial schedule.

Because EDF has no scalar priority, the preemption filter is expressed by the
branch's active set: the selected job's chain transitions run, the other
in-flight jobs' chain transitions are suspended (resume semantics, frozen
execution clock), and their release clocks keep running.

### Periodic releases

The lowered `_fire` transition consumes and re-adds the unit period token and
posts one token to the task entry. In the supported single-job class its
behavior is:

- **Idle task:** the release creates a job with a fresh zero release clock.
- **Job waiting at the entry:** the saturating capacity-1 entry place clamps
  the post arc. The marking is unchanged and the release clock is *not* reset;
  the pending job keeps its original absolute deadline. This is the
  conservative merge of an overlapping periodic release.
- **Completed job's exit token pending:** `_fire` is deferred until the
  zero-time end-of-task consumer has removed the token. The consumer is always
  enabled and consumes no time, so this preserves every timing behavior while
  preventing a new completion from merging into the pending token.
- **Job mid-chain (entry empty):** the post arc would create a second job token
  in the same chain. That is outside the one-job supported class; the analysis
  raises `std::invalid_argument` with an "ambiguous job layout" message
  instead of modeling a queue. A response time exceeding the task period is
  the typical cause.

### Restrictions and diagnostics

- `k-extrapolation` is rejected for release-clock states
  (`set_extrapolation(true)` / enabling extrapolation before installing EDF
  both throw). Relaxing deadline bounds would be unsound.
- Task-symmetry reduction is not implemented for release-clock states. For the
  single-core EDF class it would be trivial (no distinct cores to permute).
- The SCG DOT export labels release clocks `r(K<ordinal>)`; execution clocks
  remain `h(T<id>)`.
- Tests: `test/test_edf.cpp` covers known EDF response times, a case where EDF
  differs from RM/DM because of absolute deadlines, tie enumeration,
  zone-dependent branch constraints, layout invariants, and the rejection
  paths.

## When these sets are recomputed

`StateClassReachabilityGraph::recompute_sets` (`src/analysis/reachability/state_class_layout.cpp`)
is called whenever the marking changes (in `compute_initial_classes` and after
each `fire`). It:
1. Recomputes `struct_enabled` from the new marking (`Scheduling::structural_enabled`).
2. Installs one selected active set passed by the caller; the caller obtains
   every admissible set from `Scheduling::priority_branches` and creates one
   successor per set.
3. Derives `suspended = { t ∈ struct_enabled \ priority_enabled | suspendable(t) }`.

`time_elapse` does NOT change these sets — it only pushes the symbolic clock zone
forward. Suspension state is recomputed only when the marking changes.

## Resume policies: engine-native preemption

The `fixed_prior_with_resume` policy, the legacy `fixed` alias, `rm`/`dm`, the
dynamic `pip`/`pcp`/`srp` rules, and EDF rely on the sets above to model
preemption directly, instead of any structural encoding:

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

- `src/analysis/reachability/scheduling_policy.h` / `.cpp` — `FiringRule`,
  `StaticPriorityRule`, `EdfFiringRule`, `PipFiringRule`, `PcpFiringRule`,
  `SrpFiringRule`, `PolicyBranch`/`BranchConstraint`,
  `detail::TaskLockTopology`, `PolicyContext`, `make_firing_rule`
- `src/analysis/reachability/state_class.h` / `.cpp` — `ClockKind::Release`,
  `TaskChain`, `build_task_chains`, `task_in_flight`, `release_clock_of_task`
- `src/analysis/reachability/scheduling.cpp` — `structural_enabled`, `priority_branches`
- `src/analysis/reachability/state_class_layout.cpp` / `state_class_firing.cpp` —
  initial and successor branch enumeration with `PolicyContext`, release-clock
  layout/transfer, and branch-constraint intersection before hashing
- `src/analysis/reachability/ptpn_analysis.cpp` — `recompute_sets`, `time_elapse`, `is_firable`, `fire`, `build`, `build_successor_zone`, `set_firing_rule`
- `src/lower/tdg2pn/tdg2pn.cpp` — policy metadata on the PTPN, local TDG copy
- `src/lower/tdg2pn/policy_priorities.cpp` — RM/DM priority computation
- `src/lower/tdg2pn/tdg2pn_common.h` — `is_resume_policy`
- `src/lower/tdg2pn/resource_lowering.cpp` — resume-style resource lowering

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
