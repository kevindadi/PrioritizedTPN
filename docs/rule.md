# Unified TDG-to-Petri-Net P/T Rules

This document describes how TDG nodes are lowered into a static P/T Petri net under the FIFO scheduling policy. The fixed-priority variants keep the same base net and add extra preemption paths for higher-priority tasks.

## Conventions

### Places

- `entry_p`: task arrival, waiting for CPU acquisition
- `ready_p`: CPU acquired, ready to execute the next segment
- `seg_i_done_p`: state after execution segment `i`
- `hold_lock_k_p`: state after acquiring lock `k`
- `exit_p`: task completed
- `core_c_p`: CPU resource place
- `lock_x_p`: lock resource place

### Transitions

- `get_core_t`: zero-time CPU acquisition
- `lock_k_t`: zero-time lock acquisition
- `exec_i_t`: timed execution segment
- `release_t`: periodic release transition

## Rules

1. Only execution transitions and periodic release transitions carry time.
2. Lock acquisition is modeled as a separate zero-time transition.
3. Unlock is not modeled as a standalone transition; it is expressed by the output arcs of execution transitions.
4. Each execution segment maps to one timed transition.
5. The generated Petri net must keep the place-transition-place alternation.
6. The number of execution segments is `2 * lock_count + 1`.

## Task template without locks

### Aperiodic task

If `time = [C1]`:

```text
entry_p -> get_core_t -> ready_p -> exec_1_t(C1) -> exit_p
```

| Transition | Input places | Output places |
| --- | --- | --- |
| `get_core_t` | `entry_p`, `core_c_p` | `ready_p` |
| `exec_1_t` | `ready_p` | `exit_p`, `core_c_p` |

### Periodic task

The task body is lowered exactly like an aperiodic task:

```text
entry_p -> get_core_t -> ready_p -> exec_1_t(C1) -> exit_p
```

Periodic release is added separately:

```text
release_p -> release_t(P) -> release_p, entry_p
```

This release structure is created only when periodic activation is expressed by the `periodic` configuration (or a dashed binding edge). Self-loop edges are allowed in the input but are ignored by lowering.

## Task template with one lock

If `locks = [L1]`, the time segments are:

- `C1`: before the critical section
- `C2`: inside the critical section
- `C3`: after the critical section

```text
entry_p
-> get_core_t
-> ready_p
-> exec_1_t(C1)
-> seg_1_done_p
-> lock_1_t
-> hold_1_p
-> exec_2_t(C2)
-> seg_2_done_p
-> exec_3_t(C3)
-> exit_p
```

| Transition | Input places | Output places |
| --- | --- | --- |
| `get_core_t` | `entry_p`, `core_c_p` | `ready_p` |
| `exec_1_t` | `ready_p` | `seg_1_done_p` |
| `lock_1_t` | `seg_1_done_p`, `lock_L1_p` | `hold_1_p` |
| `exec_2_t` | `hold_1_p` | `seg_2_done_p`, `lock_L1_p` |
| `exec_3_t` | `seg_2_done_p` | `exit_p`, `core_c_p` |

## Task template with two nested locks

If `locks = [L1, L2]`, the assumed order is:

1. acquire `L1`
2. acquire `L2`
3. release `L2`
4. release `L1`

The time segments are:

- `C1`: before `L1`
- `C2`: after `L1`, before `L2`
- `C3`: while holding both `L1` and `L2`
- `C4`: after releasing `L2`, while still holding `L1`
- `C5`: after releasing `L1`

```text
entry_p
-> get_core_t
-> ready_p
-> exec_1_t(C1)
-> seg_1_done_p
-> lock_1_t
-> hold_1_p
-> exec_2_t(C2)
-> seg_2_done_p
-> lock_2_t
-> hold_12_p
-> exec_3_t(C3)
-> seg_3_done_p
-> exec_4_t(C4)
-> seg_4_done_p
-> exec_5_t(C5)
-> exit_p
```

| Transition | Input places | Output places |
| --- | --- | --- |
| `get_core_t` | `entry_p`, `core_c_p` | `ready_p` |
| `exec_1_t` | `ready_p` | `seg_1_done_p` |
| `lock_1_t` | `seg_1_done_p`, `lock_L1_p` | `hold_1_p` |
| `exec_2_t` | `hold_1_p` | `seg_2_done_p` |
| `lock_2_t` | `seg_2_done_p`, `lock_L2_p` | `hold_12_p` |
| `exec_3_t` | `hold_12_p` | `seg_3_done_p`, `lock_L2_p` |
| `exec_4_t` | `seg_3_done_p` | `seg_4_done_p`, `lock_L1_p` |
| `exec_5_t` | `seg_4_done_p` | `exit_p`, `core_c_p` |

## General nested-lock pattern

For lock sequence:

```text
L1, L2, ..., Ln
```

The time segments are:

```text
C1, C2, ..., C(2n+1)
```

Meaning:

- `C1`: before acquiring `L1`
- `C2`: holding `L1`, before acquiring `L2`
- ...
- `Cn`: holding `L1...L(n-1)`, before acquiring `Ln`
- `C(n+1)`: holding all `n` locks
- `C(n+2)`: after releasing `Ln`
- ...
- `C(2n)`: after releasing `L2`, still holding `L1`
- `C(2n+1)`: after releasing `L1`, until completion

Release rules:

- `exec_(n+1)_t` releases `Ln`
- `exec_(n+2)_t` releases `L(n-1)`
- ...
- `exec_(2n)_t` releases `L1`
- `exec_(2n+1)_t` releases the CPU

## Transition semantic fields

The generated Petri net uses transition fields with three distinct semantics:

- `time interval`: whether the transition consumes modeled time
- `priority`: whether the transition participates in fixed-priority scheduling order
- `core`: whether the transition is bound to a specific execution core for scheduling semantics

The lowering must treat these fields by transition role rather than assigning the same meaning to every transition.

| Transition kind | Examples | Time interval | Priority field | Core field | Suspendable | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| CPU acquisition | `get_core_t` | `I=[0,0]` | task priority | task core | `false` | Moves the task into `ready_p`. Consumes `core_c_p` only for the structural policies; the resume policy has no CPU place, so it just advances the chain. |
| Lock acquisition | `lock_k_t` | `I=[0,0]` | task priority | task core | `false` | Structural step that consumes the lock token. |
| Execution segment | `exec_i_t` | task WCET interval | task priority | task core | policy-dependent | The only task-body transition that consumes modeled execution time. |
| Periodic release | `release_t(P)` | `I=[P,P]` | none | none | `false` | Generates periodic arrivals; it is not a CPU-scheduled task execution step. |
| Dependency connector | `A_to_B` | `I=[0,0]` | none | none | `false` | Used only to preserve place-transition-place alternation for `task -> task` edges. |
| End consumer | `consume_t` | `I=[0,0]` | none | none | `false` | Removes terminal tokens from configured end tasks. |
| Restart preemption | `H_restart_preempt_L` | `I=[0,0]` | high-task priority | high-task core | `false` | Scheduling-control transition (restart policy only); preempts `L` and sends it back to `L_entry_p`. |
| Fork / join control | fork/join node transition | `I=[0,0]` | none | none | `false` | Structural synchronization transition, not a CPU execution step. |

The resume policy adds no scheduling-control transitions of its own: preemption and resume are handled by the analysis engine (see `fixed_prior_with_resume` below), so there are no `H_resume_preempt_L` / `L_resume_H` transitions.

### Rules for priority and core assignment

1. Only transitions that represent CPU-scheduled behavior carry scheduling metadata (`priority`, `core`).
2. Timed execution transitions always carry the owning task's priority and core.
3. Immediate scheduling-control transitions created for fixed-priority preemption also carry the high-priority task's priority and core.
4. Pure structural transitions do not carry scheduling semantics, even if the implementation stores placeholder fields internally.
5. `task -> task` connector transitions must not participate in preemption ordering.
6. `fork` and `join` transitions are synchronization structure, not execution on a CPU core.
7. Preemption paths are created only for task pairs on the same core.
8. A preemption-control transition must use the core of the high-priority task, which is also the core of the preempted low-priority task in valid fixed-priority expansion.
9. If the implementation requires default numeric values for non-scheduling transitions, those values are placeholders only and must not be interpreted as real scheduling policy data.

## FIFO policy

FIFO does not add extra scheduling-control places.

1. A task must enter `entry_p` before execution.
2. A task must consume a token from `core_c_p` before running.
3. Lock competition is expressed naturally by lock tokens.
4. No extra preemption path is added.
5. All execution transitions are non-suspendable by default.

## Fixed-priority variants

The fixed-priority variants keep the FIFO base task chains and add static preemption paths for higher-priority tasks on the same core.

| Aspect | FIFO | `fixed_prior_with_restart` | `fixed_prior_with_resume` |
| --- | --- | --- | --- |
| Base task chain | kept | kept | kept |
| CPU resource model | kept | kept | removed (engine per-core priority filter, bound 1 task/core) |
| Lock resource model | kept | kept | kept |
| Preemption paths | none | added (structural) | none (engine-native) |
| Low-priority token after preemption | n/a | returns to task entry | stays in place; execution clock frozen |
| Recovery after high-priority completion | n/a | low task restarts from entry | engine unfreezes the execution clock and resumes |
| `suspendable` | all `false` | lower-priority execution segments may become `true` | all execution segments `true` (except spin-lock sections) |
| Spin-lock preemption paths | n/a | not added | n/a (spin-lock sections stay non-suspendable) |

### `fixed_prior_with_restart`

This variant extends the FIFO base net with immediate preemption transitions that redirect the preempted low-priority task token back to its entry place.

```text
H_entry_p + L_preempt_p -> restart_preempt_t -> H_ready_p + L_entry_p
```

Use this when you want the cheaper static model in which a preempted task restarts from the beginning of its task chain.

Lowering notes (restart):

1. Priority is known during lowering because tasks are grouped by core and
   sorted before preemption arcs are added.
2. Only selected low-priority execution segments are marked `suspendable`.
3. `fork` and `join` nodes do not participate in preemption expansion.
4. No extra lock-specific preemption path is added once a `spin` lock is
   encountered.

### `fixed_prior_with_resume`

This variant (and the legacy `fixed` alias) no longer emits any structural
preemption sub-net, and it does not create the CPU-resource place. Preemption is
expressed entirely by the analysis engine:

- The lowering registers a parallelism bound of `1` for every real core in
  `PTPN::core_parallelism`. `Scheduling::filter_priority_per_core` then keeps at
  most one transition per core — the highest-priority structurally enabled one,
  breaking ties by the smallest transition index — which provides both CPU mutual
  exclusion (one task per core) and fixed-priority arbitration. Multi-core
  parallelism per CPU (`cores_per_cpu` > 1) is intentionally not modeled.
- A preempted (lower-priority) execution segment is marked `suspendable`, so it
  enters the suspended set. The engine freezes its execution clock during
  `time_elapse` and preserves it across firings (`build_successor_zone`); when
  the higher-priority task completes, the segment resumes from the exact frozen
  value. This supports mid-segment preemption, which the old structural sub-net
  could not.

Because there is no resource place or token-shuffling sub-net, the previous
core-token accounting deadlocks cannot occur.

Lowering notes (resume):

1. All execution segments are marked `suspendable`, except segments inside a
   `spin`-lock critical section (a spin-lock holder keeps the CPU).
2. `get_core` and lock transitions remain non-suspendable, zero-time control
   steps; `get_core` is kept only to preserve the task-chain layout.
3. `fork` and `join` nodes do not participate in preemption.

Known limitations of the engine-native model:

- Each real core is bound to a single task, so `cores_per_cpu` > 1 does not yield
  intra-CPU parallelism; the model is one task per core. When same-core
  transitions tie at the top priority, the tie is broken deterministically by
  transition index rather than explored as alternatives.
- The pure priority filter cannot express "a spin-lock holder keeps the CPU and
  cannot be preempted while spinning"; spin-lock sections are approximated by
  keeping their execution segments non-suspendable.
