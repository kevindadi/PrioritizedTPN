---
name: rt-system
description: Use when asked to model, analyze, schedule, or fix a real-time system in this repository - writing or editing RTSL, running ptpn check/rtsl/allocate, interpreting diagnostics and exit codes, or filling pending `measured by llm` task times. Covers the full clarify -> write -> check -> fill -> analyze -> report workflow for PTPN.
---

# Real-Time System Workflow

Work from the repository root. The analyzer is `build/ptpn` (`example/run.py`
resolves `--ptpn` -> `$PTPN_BUILD_DIR` -> `.ptpn-build-dir` -> `build/ptpn`).

Reference docs (use these instead of guessing syntax or codes):

- `docs/rtsl.md` - RTSL lexer/parser/validator/lowering reference, defaults,
  units, cores, locks, provenance, bindings, supported classes.
- `docs/diagnostics.md` - **authoritative** code table, severity/category,
  envelope schema, exit-code priority, and the full
  `check`/`rtsl`/`allocate` behavior matrix.
- `docs/usage.md` - commands, options, reductions, metrics fields.
- `docs/architecture.md` - pipeline/layer responsibilities.
- `docs/json_format.md` - TDG JSON input format.

## Workflow

1. **Clarify requirements before writing the model.** For every task: kind
   (`periodic`, `aperiodic`, `sporadic`, `interrupt`, `normal`), period /
   minimum inter-arrival time, deadline, priority or scheduling policy, core
   set, locks, and dependencies. Ask for the **timing source per task**:
   `certified` (safe user-supplied bound), `estimated` (informal figure), or
   `measured by llm` (a placeholder whose time is filled in later). Do not
   invent numbers.
2. **Write RTSL v1** as documented in `docs/rtsl.md` and
   `example/introduction/demo.rtsl`. The base unit is milliseconds; `us` is
   rejected (`E-UNS-004`). Periodic tasks need `period`; `sporadic` is
   rejected by validation (`E-UNS-003`). A locked task needs
   `2*lock_count+1` execution intervals.
3. **Check loop.** Run `build/ptpn check -f model.rtsl --diagnostics json`
   (omit `--diagnostics` for human-readable stderr while iterating). Fix every
   **error-severity** `E-SYN`/`E-VAL` diagnostic and rerun until exit 0.
   Hints do not block: `E-VAL-218` in `check` and `E-UNS-005` leave exit 0;
   inspect them but do not force a change just to clear a hint. On success the
   envelope's `result.pending_measurements` lists tasks still needing timing.
   `check` stops after parse+validate: it does **not** report lowering or
   analysis `E-UNS-001`, `E-VAL-015`, or the defensive `E-VAL-011`.
4. **Fill pending timing.** If `result.pending_measurements` is non-empty,
   measure the code and write the intervals with `ptpn fill` instead of editing
   the file by hand:

   ```bash
   cat > times.json <<'EOF'
   {"fusion": [[1, 2], [3, 4], [1, 2]]}
   EOF
   build/ptpn fill -f model.rtsl --values times.json --in-place
   # single task: --task fusion --intervals "[1,2],[3,4],[1,2]"
   ```

   Values are engine ticks (milliseconds); `fill` adds `ms` suffixes itself in
   a `time_unit s` model, preserves the `measured by llm { ... }` provenance
   clause, checks the interval count against the task's lock structure
   (`2 * lock_count + 1`), and re-validates the patched source before writing
   it. Errors are `E-VAL-226` (unknown task, wrong count, already filled —
   pass `--overwrite` to re-measure). Re-run
   `build/ptpn check -f model.rtsl --diagnostics json` until
   `result.pending_measurements` is empty. While tasks remain pending,
   `ptpn rtsl` and `ptpn allocate` reject the model with `E-VAL-213`.

   If you must hand-edit, keep the same shape:
   `time [ [1, 1], [1, 2], [1, 1] ] measured by llm { runs 10 }` (a wrong
   segment count is `E-VAL-015`; bare adjacent intervals are `E-SYN-102`).
   Estimated/measured values must never be presented as a certified bound; if
   the user supplies a real bound, write `certified by "<tool>"` instead.
5. **Verify the semantics (recommended before reporting).** Run
   `build/ptpn describe -f model.rtsl`: it restates the effective policy,
   priorities (including `rm`/`dm` recomputation), core mapping, lock nesting,
   release phases, and whether each dependency edge is an AND fork/join or the
   token-consuming alternative/OR form of separate statements. If the
   description does not match the user's intent, fix the model (or ask) before
   trusting any verdict.
6. **Analyze.** `build/ptpn rtsl -f model.rtsl` for WCRT/schedulability
   (add `--no-analysis` only for lowering/export). Use
   `build/ptpn allocate` when core assignment is open. Read the per-task
   `verdict` (`meets`/`misses`/`unknown`) and the report-level
   `schedulability` (`schedulable`/`unschedulable`/`unknown`) from
   `result.analysis`; `wcrt_exact: true` means the WCRT is the exact value,
   while `exact: false` or `wcrt_capped: true` means it is a bound. The
   completion observation is off under symmetry, extrapolation, merging,
   non-equality canonicalization and `--tie arbitrary`.
7. **Report** the facts, not a vibe: command, exit code, diagnostics,
   `schedulability`, `counterexample`, `exact`/`wcrt_exact`, `truncated`, and
   pending count. State timing provenance (`certified`/`measured`/`estimated`)
   and whether the result is exact. Measured or estimated intervals are **not**
   WCET bounds.

## Diagnostics -> action

Branch on `code`, `severity`, the command/stage that produced it, and the
envelope `result` — not on the code prefix alone. The complete table is
`docs/diagnostics.md`; this is the working summary:

| Signal | Action |
|---|---|
| `E-SYN-1xx` | Syntax/lexing error. Rewrite the reported line using the expected tokens; check units (`ms`, `s`) and string escapes. `E-SYN-002` is an unreadable input file, not grammar. |
| `E-VAL-2xx` | Model semantics. Fix the named task/attribute/edge. `E-VAL-213` from `rtsl`/`allocate` means measured timing is missing - fill the pending `time` attributes first. `E-VAL-226` comes from `fill`: the values do not match the model (unknown task, wrong interval count, or already filled - pass `--overwrite`). `E-VAL-015` means a locked task's segment count is wrong. `E-VAL-218` means `core any` (hint in `check`, error in `rtsl`) or a multi-element core set (hint in `check`, warning in `rtsl`, which pins the task to the smallest id): use `ptpn allocate` or an explicit single core. Non-blocking schedule hints: `E-VAL-222` (deadline > period), `E-VAL-223` (same core and priority), `E-VAL-224` (single-core utilisation > 1), `E-VAL-225` (several separate edge statements sharing a source or target: the targets are alternatives / the target is released by whichever source completes first). |
| `E-UNS-001` | Supported-class guard from lowering **or analysis** (not emitted by `check`). EDF/FIFO/LLF require a static partition, `cores_per_cpu 1`, one in-flight job per task, no locks, no fork/join, and no dependency-driven releases; PIP/PCP/SRP require `task_place_capacity 1`. Analysis also rejects job-clock overlapping releases (period shorter than execution) and job-clock + extrapolation/symmetry. Subjects are `task:`, `node:`, `edge:`, `policy:`, `option:extrapolation`, or `option:symmetry`. Simplify the model or change the policy/option. |
| `E-UNS-003` | Sporadic arrivals are not implemented and stay rejected. With the user's explicit agreement, you may model a **periodic reference scenario** with an explicit period such as `period 10ms`. That changes the model and does not certify the original sporadic system; never auto-change the task kind, delete `mit`, or rewrite the period to clear the diagnostic. |
| `E-UNS-004` | Sub-millisecond (`us`) timing is unsupported. Use `ms` and adjust precision. |
| `E-UNS-005` | `interrupt` is only a hint: it is lowered as a non-periodic task with a type marker; no interrupt-preemption semantics exist. Do not claim interrupt support. |
| `E-PERF-001` | `-m` max-states truncation. If a counterexample is already reported, **do not raise `-m`** - fix the design. Otherwise raising `-m` may help; the result is partial either way. |
| `E-PERF-002` | `--horizon` truncation. Same rule: with a counterexample, change the design; without one, raise/disable the horizon. |
| `E-PERF-003/004` | The `--timeout` / `--max-memory` budget stopped the exploration. Results are partial (exit 5); raise the budget or simplify the model. |
| any other code | `build/ptpn explain CODE` prints the documented trigger, subject and fix (the catalog is generated from `docs/diagnostics.md`, so it cannot go stale). |
| `R-ANA-001` | Certified reachable deadlock. Fix dependency/lock structure or policy. |
| `R-ANA-002` | A proven deadline miss (verdict `misses`): the certified lower bound exceeds the deadline, the observed response reached the deadline cap (`wcrt_capped`), or a complete graph shows the job never completes. Change priorities, cores, or periods; `result.analysis.tasks[].witness` shows the worst-case path. |
| `R-ANA-003` | Periodic local deadlock. Change period, priority, or core assignment. |
| `R-ANA-004` | **Hint**: the task is never released (no `start` binding and no incoming edge), so it never executes. Add a release or remove the task. |
| `E-INT-*` | Typed supported-class rejections use `E-UNS-001`; unclassified internal failures use `E-INT-*`. |

Use `code`, `category`, `severity`, `subject`, `location`, and `exit_code` for
branching. `message` and `fix` are informational. Hints (for example
`E-VAL-218` in `check`, `E-UNS-005`) leave the exit code at 0; warnings alone
usually do too, but `E-PERF-*` warnings still produce exit 5.

## Command-stage matrix

| Situation | `check` | `rtsl` | `allocate` |
|---|---|---|---|
| pending `measured` timing | success; `result.pending_measurements` | `E-VAL-213`, exit 3 | `E-VAL-213`, exit 3 |
| `core any` | `E-VAL-218` hint, exit 0 | `E-VAL-218` error, exit 3 | expands the domain; no diagnostic |
| `interrupt` | `E-UNS-005` hint, exit 0 | hint, lowered as non-periodic | hint |
| locked segment-count mismatch | not checked | `E-VAL-015`, exit 3 | `E-VAL-015`, exit 3 |
| EDF/FIFO/LLF supported-class violation | not checked | `E-UNS-001`, exit 4 | candidate infeasible; no `E-UNS-001` |

## Exit codes

| Exit | Meaning |
|---|---|
| `0` | Success. |
| `2` | Syntax error (`E-SYN-`). |
| `3` | Validation error (`E-VAL-`). |
| `4` | Unsupported model/policy/feature (`E-UNS-`). |
| `6` | Analysis counterexample: deadlock, deadline miss, or local deadlock (`R-ANA-`). |
| `5` | Partial results: truncation with no counterexample (`E-PERF-`). |
| `1` | Internal error (`E-INT-`). |

Category priority is `2 > 3 > 4 > 6 > 5 > 0`; validation warnings alone do not
change the exit code; `E-INT-` dominates. CLI11 usage errors (missing required
option, unknown `--policy`, invalid `--diagnostics` value) bypass the envelope
and use CLI11's own codes (for example 106/105). **Never raise `-m` on a proven
counterexample** (exit 6, or `E-PERF-*` accompanied by a counterexample):
truncation does not invalidate a counterexample found in the explored prefix.
A max-states cut no longer reports deadlocks from dropped successors, so a
truncated run without a proven miss exits 5, not 6. Conversely, an upper-bound
`wcrt` exceeding a deadline is not a counterexample and never produces exit 6;
`R-ANA-002` requires the task verdict to be `misses` (a certified lower bound
above the deadline, a capped observation, or proven starvation).

## Trust boundary and design changes

- Estimated and measured values are not certified WCET bounds. Never present
  an estimate (including `estimated by "llm"`) as a hard real-time guarantee;
  only a bound the user supplied as `certified by "<tool>"` carries that claim.
- Report analyzer output mechanically; do not claim schedulability that the
  exit code and `exact`/`truncated` fields do not support.
- **Equal-priority tasks are common.** Every task defaults to `priority 100`,
  so tasks without an explicit `priority` are peers. The default `--tie fifo`
  never lets an equal-priority peer preempt a running transition or an
  in-service job; `--tie arbitrary` explores equal-priority preemption (worst
  case over tie-breaks) and can report starvation that FIFO would not. State
  which tie mode produced a verdict before presenting it as the system's
  behavior.
- **Design changes require the user's decision.** Deleting a lock or
  dependency, changing the policy, or changing a period/deadline changes the
  model's semantics. Show the exact diff and the before/after diagnostics and
  metrics, and let the user decide. Do not silently rewrite the design to make
  a diagnostic disappear.
- **Spin locks are not a reliable preemption guarantee.** The current lowering
  marks the wrong execution segment for a single spin lock (the pre-critical
  segment is non-suspendable and the critical section is suspendable). Do not
  claim that spin-lock critical sections keep the CPU; see `docs/rtsl.md` §7
  and `example/diagnostics/spin-lock.rtsl`.
