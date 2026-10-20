# RTSL v1 reference

RTSL is the unit-aware, declarative front-end for the PTPN analyzer. A `.rtsl`
file is lexed, parsed with panic-mode recovery, validated, and lowered
in-process to a TDG; no JSON text round-trip is involved. The same TDG then
runs through the shared lowering/analysis pipeline.

This page describes the **implemented** language. Where a construct is accepted
by the grammar but has no runtime semantics yet, that is called out explicitly
(see [§11](#11-supported-classes-and-limitations)). Diagnostic codes and
actions are in [diagnostics.md](diagnostics.md); the CLI workflow is in
[usage.md](usage.md). The complete grammar is [rtsl.ebnf](rtsl.ebnf)
(keywords are checked against the lexer by `test/test_rtsl_grammar.cpp`), which
is the compact reference to hand to a code generator or an LLM.

## 1. Lexical rules

| Element | Rule |
|---|---|
| Whitespace | space, tab, CR, LF; insignificant between tokens |
| Line comment | `//` to end of line |
| Block comment | `/* ... */`; an unterminated block is `E-SYN-101` |
| Identifier | `[A-Za-z_][A-Za-z0-9_]*`; case-sensitive |
| Integer | decimal digits only, non-negative, 64-bit (no sign, no float) |
| Time literal | integer immediately followed by letters, for example `10ms`, `2s`, `5us` |
| String | double-quoted, may not contain a raw newline; escapes `\"`, `\\`, `\n` |
| Punctuation | `{ } [ ] , : ; ->` |
| Statement separator | `;` is optional and skipped at system/task level |
| Keywords | `system cpus cores_per_cpu time_unit policy task normal periodic aperiodic sporadic interrupt period deadline mit priority core any locks time measured estimated certified by llm runs timeout artifact start end`; keywords are reserved and cannot be used as identifiers |

Policy names (`fixed`, `rm`, `edf`, ...) are **not** keywords: they lex as
identifiers and are validated semantically. Identifiers may not start with a
digit. Negative numbers are not part of the grammar; use non-negative values.

## 2. Program structure

A program is exactly one `system` block:

```rtsl
system NAME {
  cpus 2
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task NAME: KIND {
    ...attributes...
  }

  start NAME
  end NAME
  A -> B
  A -> {B, C}
  {A, B} -> C
}
```

The system name is a required identifier. The parser reads the system block
and stops at its closing brace; content after it is not parsed. Attributes and
declarations inside the system block may appear in any order. Duplicate
attributes are reported as `E-VAL-219`; lowering uses the first declaration for
system and task attributes.

## 3. System block

| Attribute | Required | Values | Default | Notes |
|---|---|---|---|---|
| `cpus N` | yes | integer `>= 1` | — | number of real cores; valid core ids are `0..N-1` |
| `cores_per_cpu N` | no | integer `>= 1` | `1` | capacity parameter; EDF/FIFO/LLF require `1` |
| `time_unit U` | no | `ms` or `s` | `ms` | base unit for bare time values; `us` is `E-UNS-004` |
| `policy P` | no | policy name | `fixed` | see [usage.md](usage.md#scheduling-policies) |

`cores_per_cpu` is **not** an extra range of core ids and does not create
additional real cores; it is a capacity parameter of each core.

## 4. Tasks

```rtsl
task NAME: KIND {
  period TIME
  deadline TIME
  priority N
  core 0
  core {0, 1}
  core any
  locks mutex1, mutex2
  time [min, max] estimated by "note"
  time [ [min, max], [min, max], [min, max] ] measured by llm { runs 10, timeout 2 }
}
```

- `KIND` is optional; the default is `normal`.
- Kinds: `normal`, `periodic`, `aperiodic`, `sporadic`, `interrupt`.
- Task ids must not contain `__` (reserved for implicit fork/join nodes,
  `E-VAL-202`); duplicate ids are `E-VAL-201`.

| Attribute | Applies to | Rules | Default when omitted |
|---|---|---|---|
| `period TIME` | `periodic` only | required for periodic; raw value `> 0` (`E-VAL-206`) | — |
| `deadline TIME` | any task | raw value `> 0` (`E-VAL-208`); **relative** deadline measured from the job's release (a response-time budget), not an absolute time | `0` = implicit deadline (the period) for periodic tasks |
| `mit TIME` | `sporadic` only | required for sporadic; raw value `> 0` (`E-VAL-207`) | — |
| `priority N` | any task | integer in `[-2147483648, 2147483647]` (`E-VAL-221` outside it); **higher value = higher scheduling priority** | `100` |
| `core ...` | any task | single id, set `{...}`, or `any`; range `0..cpus-1` | core `0` |
| `locks ...` | any task | one or more identifiers, comma-separated | no locks |
| `time ...` | any task | required; intervals plus provenance | — |

`period` on a non-periodic task and `mit` on a non-sporadic task are
`E-VAL-205`. A missing `time` is `E-VAL-211`. Attribute meanings during
analysis:

- `priority` is the fixed priority for `fixed`/`fixed_prior_*`; `rm`/`dm`
  **recompute** priorities during lowering (rate/deadline monotonic), and
  PIP/PCP/SRP derive effective priorities dynamically.
- `deadline` is absolute in engine ticks. `0`/absent means the implicit
  deadline (the period) for periodic tasks; one-shot tasks without a deadline
  have no deadline check.
- `period` creates a periodic release transition (`{task}_fire`), one job every
  period, first at `P` (not at time 0 unless the task is also `start`ed).
  Task-chain places saturate at capacity 1 in RTSL lowering, so a release that
  arrives while a job is pending merges instead of queueing.
- `normal` and `aperiodic` are both one-shot tasks: they need a `start` binding
  or a dependency edge to be released. `aperiodic` is not an arrival-process
  model; it is simply a non-periodic task.

## 5. Time syntax and normalization

A time value is a non-negative integer with an optional suffix:

```rtsl
time_unit ms
time [2, 3] estimated          // 2 ms .. 3 ms
time [2ms, 3ms] estimated      // same
time [1s, 2s] estimated        // 1000 ms .. 2000 ms
time [1us, 2us] estimated      // E-UNS-004 (sub-millisecond unsupported)
```

Rules:

- A bare integer uses the **system `time_unit`**, default `ms`.
- `ms` and `s` suffixes are absolute and override the system unit. `s` is
  multiplied by 1000.
- `us` is rejected with `E-UNS-004`, both as `time_unit us` and as a value
  suffix. Unknown suffixes are `E-SYN-101`.
- A value that normalizes to `INT_MAX` ms or more does not fit the engine's
  signed 32-bit tick range and is rejected with `E-VAL-221` (the same applies to
  `priority`, `cpus`, and `cores_per_cpu`). `INT_MAX` itself is reserved as the
  +infinity sentinel.
- The engine tick is **1 ms** for RTSL input. TDG JSON integers remain
  unit-agnostic engine ticks.
- After normalization, each interval must satisfy `min <= max`
  (`E-VAL-212`); `min == max` is allowed (a deterministic segment).

Interval forms:

```rtsl
time [2, 3] estimated                                  // one segment
time [ [1, 1], [2, 3], [1, 1] ] estimated               // three segments
```

The outer `[...]` is required. A single interval is `[min, max]`; multiple
segments are a comma-separated list of `[min, max]` intervals. A nested list
with one interval (`[[1,2]]`) is accepted. Bare adjacent intervals such as
`[1, 2] [3, 4]` are a syntax error (`E-SYN-102`), as is an empty `[]`.
Trailing commas are not accepted in any list. The comma between `min` and
`max` is mandatory.

## 6. Cores

```rtsl
core 0          // one core
core {1, 2, 3}  // allowed set; lowering selects the smallest core
core any        // allocation domain 0..cpus-1
```

- The core id range is `0..cpus-1` (`E-VAL-209`). The control core `-1` is an
  internal PTPN concept and cannot be written in RTSL.
- A core set must be non-empty and free of duplicates (`E-VAL-210`).
- For a set, lowering sets `core` to the smallest id and records the full set
  as `allowed_cores`; `ptpn allocate` explores the set. For `core any`,
  lowering expands the domain to every core.
- `core any` is a **hint** in `ptpn check`, an **error** in `ptpn rtsl`, and
  expanded by `ptpn allocate` (`E-VAL-218` in the first two modes). A
  multi-element set is a **hint** in `check` and a **warning** in `rtsl`
  (`E-VAL-218`, exit `0`): analysis would pin the task to the smallest id, so
  use `ptpn allocate` to explore the set. `allocate` expands both silently.
- If `core` is absent, the task uses core `0`.

## 7. Locks and execution segments

```rtsl
task fusion: aperiodic {
  core 1
  locks mutex1
  time [ [1, 1], [1, 2], [1, 1] ] estimated by "llm"
}
```

- Lock names must start with `mutex` or `spin` (`E-VAL-014`) and must not
  contain `__` (`E-VAL-202`). Locks are implicitly declared by the tasks that
  use them; there is no separate system-level lock declaration.
- A task with `L` locks must declare exactly `2*L + 1` execution intervals:
  a pre-critical segment, the critical section, and a post-critical segment per
  lock, plus the final tail. One lock therefore needs three intervals; two
  locks need five. The rule is enforced by lowering (`E-VAL-015`), so
  `ptpn check` does not report it.
- Locks are acquired in the order listed and released in reverse (LIFO, nested
  critical sections). The first listed lock is the outermost. There is no RTSL
  syntax for non-nested or partially overlapping critical sections.
- Lock names are global: two tasks that list `mutex1` contend on the same lock.
  Duplicate names within a task are deduplicated in declaration order; no
  duplicate-lock diagnostic is emitted.
- **Spin-lock critical sections keep the CPU.** A task's `spin` lock marks
  every execution segment that runs inside its critical section as
  non-suspendable (the analysis cannot preempt or freeze those segments), so a
  spin lock is never held across a preemption. With `L` locks acquired in
  order and released in reverse, lock `i` is held from the end of segment `i`
  until the end of the segment that returns it, which covers the nested
  critical sections described above. `mutex` locks may suspend; the analysis
  then applies the selected policy (for example PIP/PCP/SRP priority
  inheritance).
  Evidence: `example/diagnostics/spin-lock.rtsl` lowers to `worker_exec_1`
  suspendable, `worker_exec_2` (the critical section) non-suspendable and
  `worker_exec_3` suspendable; the lock arcs
  `worker_seg_1_done -> worker_lock_1`, `spin1 -> worker_lock_1`,
  `worker_hold_1 -> worker_exec_2`, `worker_exec_2 -> spin1` enclose exactly
  that segment.
- `locks` may appear once per task; multiple `locks` attributes are
  `E-VAL-219`. Multiple names in one `locks` list are allowed.

## 8. Timing provenance and pending measurements

Every `time` declaration must end with a provenance clause:

```rtsl
time [2, 3] estimated
time [2, 3] estimated by "datasheet"
time [2, 3] certified by "aiT" { artifact "report.json" }
time measured by llm { runs 10, timeout 2 }
time [ [1, 1], [1, 2], [1, 1] ] measured by llm { runs 20, timeout 5, artifact "trace.bin" }
```

| Clause | Intervals | Meaning |
|---|---|---|
| `estimated [by "note"]` | required (`E-VAL-214`) | informal estimate; method defaults to `"user estimate"` |
| `certified by "tool" [{ artifact "path" }]` | required (`E-VAL-214`) | a user/tool-supplied safe bound |
| `measured by llm { ... }` | optional | observed measurement; if absent, the task is **pending** |

`measured by llm { ... }` accepts `runs N`, `timeout N` (wall-clock seconds)
and `artifact "path"`, comma-separated; at least one item is required. Missing
`runs`/`timeout` default to `20`/`5`. `runs` and `timeout` must be positive
(`E-VAL-220`).

### 8.1 Pending workflow

A `measured by llm` declaration **without intervals** is pending:

```bash
./build/ptpn check -f model.rtsl --diagnostics json
# exit 0, result.pending_measurements:
# [{"task":"fusion","expected_segments":3,"runs":10,"timeout":2,
#   "inject":{"file":"model.rtsl","byte_start":...,"byte_end":...,"line":26,"column":5}}]
```

`expected_segments` is `2*lock_count+1`; `inject` is the byte span of the whole
`time` attribute. `ptpn fill` uses that span to rewrite the source for you:

```bash
cat > times.json <<'EOF'
{"fusion": [[1, 2], [3, 4], [1, 2]]}
EOF
./build/ptpn fill -f model.rtsl --values times.json --in-place
# or: --task fusion --intervals "[1,2],[3,4],[1,2]"
# or: -o filled.rtsl / plain stdout when neither is given
```

`fill` values are engine ticks (milliseconds); in a `time_unit s` model they
are written with explicit `ms` suffixes. The provenance clause (`runs`,
`timeout`, `artifact`) is preserved verbatim, the interval count is checked
against the task's lock structure, and the patched source is re-validated
before it is written, so `fill` never produces an invalid model. `--overwrite`
replaces intervals that are already present; without it, filling a task twice
is `E-VAL-226`.

```rtsl
// before
time measured by llm { runs 10, timeout 2 }
// after (one lock -> three segments)
time [ [1, 1], [1, 2], [1, 1] ] measured by llm { runs 10, timeout 2 }
```

Hand-editing works too (that is the form above). Use `certified by "<tool>"`
only for a real bound. **Estimated and measured values are not certified WCET
bounds.** While a task is pending, `ptpn check` succeeds but `ptpn rtsl` and
`ptpn allocate` reject the model with `E-VAL-213`.

## 9. Start/end bindings, dependencies, fork/join

```rtsl
start sensor
end display

sensor -> {fusion, logger}     // fork: sensor releases fusion and logger
{fusion, logger} -> display    // join: display waits for both
```

- `start NAME` puts one initial job token into the task's entry place at time
  0. RTSL has no token-count syntax; the count is always 1. A periodic task
  can be both started and periodically released (one initial job plus releases
  at `P, 2P, ...`).
- `end NAME` adds a consume transition after the task's exit place. Lowering
  also adds a consume transition to every task with no non-self successor, so
  `end` is an explicit annotation of a sink. On a non-sink task the explicit
  `end` competes with the outgoing dependency edge (the exit token can be
  consumed or continue); this is not diagnosed.
- `A -> B` is a dependency edge. The target is released when the source
  completes; lowering inserts a zero-time bridge transition. Self-loops are
  accepted but are annotations, not dependency releases. RTSL has no dashed
  edges or edge labels.
- Several **separate** statements that share a source or target use the
  token-consuming semantics: `a -> b` plus `a -> c` makes `b` and `c`
  alternatives (the exit token is consumed by whichever bridge fires first),
  and `a -> c` plus `b -> c` releases `c` when whichever source completes first
  (once per completion). Use one statement (`a -> {b, c}` / `{a, b} -> c`) when
  you mean an AND fork/join; `ptpn check`/`rtsl` hint this with `E-VAL-225` and
  `ptpn describe` restates the effective semantics.
- `A -> {B, C}` creates an implicit fork node
  `__fork_<A>__<B>__<C>` (targets sorted). The fork is a zero-time control
  transition on the control core; each target gets its own token.
- `{A, B} -> C` creates an implicit join node
  `__join_<A>__<B>__<C>` (sources sorted); the join fires only after all
  sources complete, then releases the target.
- Multi-source **and** multi-target edges (`{A,B} -> {C,D}`) are rejected with
  `E-VAL-216`; split them through an explicit task.
- Edge endpoints and bindings must name declared tasks (`E-VAL-204`).
- Periodic tasks are released by their `_fire` transition; a dependency edge
  into a periodic task adds a release condition. EDF/FIFO/LLF reject
  dependency-driven releases (see below).

Lowering is deterministic: tasks are emitted in sorted id order, implicit
fork/join names are canonicalized, and edges are deduplicated and sorted, so
declaration order does not change the exported TDG.

## 10. Scheduling policies

`policy` accepts `fixed`, `fixed_prior_with_resume`,
`fixed_prior_with_restart`, `rm`, `dm`, `edf`, `fifo`, `llf`, `pip`, `pcp`,
and `srp`. `fixed` is an alias treated as resume-style. Unknown names are
`E-VAL-215`. Policy semantics and the JSON equivalents are documented in
[usage.md](usage.md#scheduling-policies); the run-time supported class is
enforced during lowering and reported as `E-UNS-001` by `ptpn rtsl`
(**not** by `ptpn check`).

`--policy` on the command line overrides the TDG policy **after** RTSL
lowering and does not re-run RTSL validation. RM/DM priorities are computed
inside TDG2PN, so `--export-tdg` materializes the source priorities, not the
computed rank. `priority` is used directly by `fixed*`, `pip`, `pcp`, and
`srp`; it is ignored by `rm`/`dm` (recomputed) and unused by EDF/FIFO/LLF
(clock-based ordering).

### Equal-priority ties

Every task defaults to `priority 100`, so tasks that do not set `priority`
are **all equal-priority**. For the fixed-priority policies (`fixed*`, `rm`,
`dm`, `pip`, `pcp`, `srp`) the tie handling is selected by the CLI option
`--tie`:

- `fifo` (default): an equal-priority peer does **not** preempt a transition
  that is already running, and a job already in service (its chain token left
  the entry place) is preferred over a job waiting for its first dispatch.
  The task that starts first therefore runs to completion (or until it
  blocks), matching the non-preemptive same-priority behavior of most RTOSes.
  Simultaneous releases (for example several `start`ed tasks) are still
  ambiguous and both dispatch orders are explored.
- `arbitrary`: every equal-priority choice is enumerated, including
  preemption by a peer. This is the worst case over tie-breaks and can report
  a real system as unschedulable (or starve a task) when a FIFO tie-break
  would not.

EDF/FIFO/LLF order jobs by clocks, not scalar priorities; their equal-deadline
ties are separate branches and `--tie` does not apply.

## 11. Supported classes and limitations

The grammar accepts more than the analyzer implements. Distinguish three
levels:

| Construct | Syntax accepted | Validated | Runtime semantics |
|---|---|---|---|
| `normal`, `aperiodic` | yes | yes | one-shot task, released by `start` or a dependency edge |
| `periodic` | yes | requires `period` | periodic release every period |
| `sporadic` | yes | **error** `E-UNS-003` | none; lowering skips it. With the user's agreement you may model a periodic reference scenario with an explicit period such as `period 10ms`. That is a different model and does not certify the original sporadic system |
| `interrupt` | yes | **hint** `E-UNS-005` | lowered as a non-periodic task with `TaskType::INTERRUPT`; **no interrupt-preemption semantics** yet. The marker currently has no analysis effect |
| `core any` | yes | hint/error depending on command | allocation domain expansion in `ptpn allocate` only |
| locks | yes | prefix/count rules | mutex/spin locking; segment count enforced by lowering |
| `estimated`/`certified`/`measured` | yes | interval requirements | timing provenance recorded in the TDG; pending workflow |
| `start`/`end` | yes | task reference | initial token / consume transition |
| edges, fork/join | yes | reference and multi-multi rules | dependency releases and synchronization nodes |

Policy-specific restrictions (reported as `E-UNS-001` by `ptpn rtsl`; most are
detected during lowering, while the job-overlap and extrapolation/symmetry
combinations are detected during analysis):

- **fixed / fixed_prior_with_resume / fixed_prior_with_restart / rm / dm /
  pip / pcp / srp**: RTSL always produces `task_place_capacity == 1`, so the
  PIP/PCP/SRP capacity guard is satisfied. PIP/PCP/SRP infer ownership from
  the single job token; a start binding above one job is rejected (RTSL always
  uses one). PCP/SRP use a global system ceiling (the classical protocols are
  uniprocessor).
- **edf / fifo / llf**: statically partitioned multi-core only — every task on
  a fixed core, `cores_per_cpu 1`, one in-flight job per task, no locks, no
  fork/join nodes, and no task-to-task dependency releases. Every task must be
  either periodic or `start`-bound. EDF and LLF additionally require a positive
  explicit-or-implicit deadline for every task; FIFO has no deadline
  requirement. EDF/FIFO use release clocks; LLF requires deterministic
  (zero-width) execution segments and is event-driven LLF, not classical
  continuous-time LLF.
- **pip / pcp / srp**: supported on TDG2PN-lowered nets with the constraints
  above; locks are required for the protocols to matter. See
  [usage.md](usage.md#scheduling-policies).

`ptpn check` validates syntax and semantics only. It does **not** run lowering,
so it does not report `E-UNS-001`, `E-VAL-015` (segment count), or the
defensive `E-VAL-011` check. Use `ptpn rtsl` (or `ptpn allocate`) to test the
supported class.

## 12. Complete example

The committed [demo.rtsl](../example/introduction/demo.rtsl) has a periodic
producer, an aperiodic consumer with a lock, a fork/join, and one pending
measurement:

```rtsl
system demo {
  cpus 3
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task sensor: periodic {
    period 100ms
    deadline 100ms
    core 0
    time [2, 3] estimated by "datasheet"
  }

  task fusion: aperiodic {
    core 1
    locks mutex1
    time measured by llm { runs 10, timeout 2 }
  }

  task logger: aperiodic {
    core 2
    time [1, 2] estimated
  }

  task display: normal {
    core 2
    time [1, 1] estimated
  }

  start sensor
  end display

  sensor -> {fusion, logger}
  {fusion, logger} -> display
}
```

A runnable filled version is
[`example/introduction/demo-filled.rtsl`](../example/introduction/demo-filled.rtsl);
it replaces the pending `fusion` timing with three locked segments:

```bash
./build/ptpn check -f example/introduction/demo.rtsl --diagnostics json  # exit 0, pending fusion
./build/ptpn rtsl  -f example/introduction/demo.rtsl --diagnostics json  # exit 3, E-VAL-213
./build/ptpn rtsl  -f example/introduction/demo-filled.rtsl --diagnostics json \
    --export-tdg /tmp/demo.tdg.json --export-metrics /tmp/demo.metrics.json
./build/ptpn tdg   -f /tmp/demo.tdg.json --export-metrics /tmp/demo-tdg.metrics.json
# the two metrics files are identical
```

More runnable snippets:

```bash
# Open core assignment
./build/ptpn check    -f example/introduction/core-any.rtsl --diagnostics json  # hint, exit 0
./build/ptpn rtsl     -f example/introduction/core-any.rtsl --diagnostics json  # error, exit 3
./build/ptpn allocate -f example/introduction/core-any.rtsl -o /tmp/alloc.json \
    --diagnostics json                                                          # exit 0

# Single lock, three segments
./build/ptpn rtsl -f example/introduction/locked.rtsl --diagnostics json

# Representative errors
./build/ptpn check -f example/diagnostics/syntax-error.rtsl --diagnostics json      # exit 2
./build/ptpn check -f example/diagnostics/validation-error.rtsl --diagnostics json  # exit 3
./build/ptpn rtsl  -f example/diagnostics/unsupported-edf-locks.rtsl --diagnostics json  # exit 4
```

| Example | Command | Expected result |
|---|---|---|
| `demo.rtsl` | `check --diagnostics json` | exit `0`; no diagnostics; `fusion` in `result.pending_measurements` |
| `demo.rtsl` | `rtsl --diagnostics json` | exit `3`; `E-VAL-213` error, subject `task:fusion:time` |
| `demo-filled.rtsl` | `rtsl --diagnostics json` | exit `0`; no diagnostics; metrics identical to the round-tripped TDG |
| `core-any.rtsl` | `check --diagnostics json` | exit `0`; `E-VAL-218` hint (one per task) |
| `core-any.rtsl` | `rtsl --diagnostics json` | exit `3`; `E-VAL-218` error |
| `core-any.rtsl` | `allocate ... --diagnostics json` | exit `0`; domain expanded, no `E-VAL-218` |
| `locked.rtsl` | `rtsl --diagnostics json` | exit `0`; no diagnostics |
| `diagnostics/syntax-error.rtsl` | `check --diagnostics json` | exit `2`; `E-SYN-102` error |
| `diagnostics/validation-error.rtsl` | `check --diagnostics json` | exit `3`; `E-VAL-206` error |
| `diagnostics/unsupported-edf-locks.rtsl` | `check --diagnostics json` | exit `0`; no diagnostics (no lowering) |
| `diagnostics/unsupported-edf-locks.rtsl` | `rtsl --diagnostics json` | exit `4`; `E-UNS-001` error, subject `task:a` |

The timings in `demo.rtsl`/`demo-filled.rtsl` are illustrative estimates
(`estimated by "datasheet"`/`"llm"`), not certified WCET bounds. The examples
are meant to exercise the workflow and diagnostics, not to certify a real
system. The precise message, fix and location columns are in
[diagnostics.md](diagnostics.md).

### 12.1 Sporadic reference scenario

`sporadic` remains rejected. If (and only if) the user agrees to change the
model, a periodic reference scenario can be written with real RTSL syntax:

```rtsl
// example/diagnostics/sporadic-reference.rtsl
task sp: periodic {
  period 10ms      // explicit period; this is not `period = mit`
  deadline 10ms
  core 0
  time [1, 2] estimated
}
```

This is a periodic task, not the original sporadic task. It can be analyzed,
but its WCRT/schedulability results are for the reference model only and must
not be presented as certification of the sporadic system. Do not let a tool
silently change the task kind, delete `mit`, or rewrite the period to make
`E-UNS-003` disappear.

### 12.2 Spin-lock evidence

`example/diagnostics/spin-lock.rtsl` is the minimal single-spin-lock model.
The reproduction command is:

```bash
./build/ptpn rtsl -f example/diagnostics/spin-lock.rtsl \
    --debug --export-ptpn /tmp/spin.ptpn.dot
```

In the exported DOT the execution transitions are colour-coded: `worker_exec_2`
(the critical section, the segment enclosed by `worker_hold_1 ->
worker_exec_2` and `worker_exec_2 -> spin1`) is drawn non-suspendable, while
`worker_exec_1` and `worker_exec_3` are suspendable. Replacing `spin1` with
`mutex1` makes all three segments suspendable.

## 13. Related documentation

- [usage.md](usage.md) — commands, options, reductions, metrics fields.
- [diagnostics.md](diagnostics.md) — code table, envelope, exit codes, actions.
- [architecture.md](architecture.md) — front-end/lowering/analysis layers.
- [json_format.md](json_format.md) — TDG JSON for advanced models (explicit
  fork/join attributes, `allowed_cores`, `task_place_capacity`, edge style).
