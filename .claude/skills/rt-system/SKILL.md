---
name: rt-system
description: Use when asked to model, analyze, schedule, or fix a real-time system in this repository - writing or editing RTSL, running ptpn check/rtsl/allocate, interpreting diagnostics and exit codes, or filling pending `measured by llm` task times. Covers the full clarify -> write -> check -> fill -> analyze -> report workflow for PTPN.
---

# Real-Time System Workflow

Work from the repository root. The analyzer is `build/ptpn` (`example/run.py`
resolves `--ptpn` -> `$PTPN_BUILD_DIR` -> `.ptpn-build-dir` -> `build/ptpn`).
Reference docs: `docs/usage.md`, `docs/install.md`, `docs/json_format.md`.

## Workflow

1. **Clarify requirements before writing the model.** For every task: kind
   (`periodic`, `aperiodic`, `sporadic`, `interrupt`, `normal`), period /
   minimum inter-arrival time, deadline, priority or scheduling policy, core
   set, locks, and dependencies. Ask for the **timing source per task**:
   `certified` (safe user-supplied bound), `estimated` (informal figure), or
   `measured by llm` (a placeholder whose time is filled in later). Do not
   invent numbers.
2. **Write RTSL v1** exactly as shown in the RTSL workflow section of
   `docs/usage.md` and `example/introduction/demo.rtsl`. The base unit is
   milliseconds; `us` is rejected (`E-UNS-004`). Periodic tasks need `period`;
   sporadic tasks are unsupported in v1 (`E-UNS-003`).
3. **Check loop.** Run `build/ptpn check -f model.rtsl --diagnostics json`
   (omit `--diagnostics` for human-readable stderr while iterating). Fix every
   `E-SYN`/`E-VAL` diagnostic and rerun until exit 0. On success the
   envelope's `result.pending_measurements` lists tasks still needing timing.
4. **Fill pending timing.** If `result.pending_measurements` is non-empty,
   fill each listed task directly in the `.rtsl` source: replace
   `time measured by llm { ... }` with an interval such as
   `time [2, 4] estimated by "llm"`. Estimated values must never be presented
   as a certified bound; if the user supplies a real bound, write
   `certified by "<tool>"` instead. Re-run
   `build/ptpn check -f model.rtsl --diagnostics json` until
   `result.pending_measurements` is empty. While tasks remain pending,
   `ptpn rtsl` and `ptpn allocate` reject the model with `E-VAL-213`.
5. **Analyze.** `build/ptpn rtsl -f model.rtsl` for WCRT/schedulability
   (add `--no-analysis` only for lowering/export). Use
   `build/ptpn allocate` when core assignment is open. Read the metrics'
   `exact` flag: `false` under symmetry, extrapolation, or horizon truncation;
   `wcrt_lb` is absent when only an upper bound exists.
6. **Report** the facts, not a vibe: command, exit code, diagnostics,
   `schedulable`, `counterexample`, `exact`, `truncated`, and pending count.
   State timing provenance (`certified`/`measured`/`estimated`) and whether the
   result is exact. Measured or estimated intervals are **not** WCET bounds.

## Diagnostics -> action

| Code | Action |
|---|---|
| `E-SYN-1xx` | Syntax/lexing error. Rewrite the reported line using the expected tokens; check units (`ms`, `s`) and string escapes. |
| `E-VAL-2xx` | Model semantics. Fix the named task/attribute/edge. `E-VAL-213` from `rtsl`/`allocate` means measured timing is missing - fill the pending `time` attributes first. `E-VAL-218` (`core any`) means use `ptpn allocate` or an explicit core. |
| `E-UNS-001` | The model is outside the chosen policy's supported class. EDF/FIFO/LLF require a static partition, `cores_per_cpu 1`, one in-flight job per task, no locks, and no dependency-driven releases; PIP/PCP/SRP require `task_place_capacity 1`. Simplify the model or change policy. |
| `E-UNS-003` | Sporadic arrivals are not implemented. Replace with `periodic` using `period = mit` (densest arrival; conservative WCRT upper bound). |
| `E-UNS-004` | Sub-millisecond (`us`) timing is unsupported. Use `ms` and adjust precision. |
| `E-PERF-001` | `-m` max-states truncation. If a counterexample is already reported, **do not raise `-m`** - fix the design. Otherwise raising `-m` may help; the result is partial either way. |
| `E-PERF-002` | `--horizon` truncation. Same rule: with a counterexample, change the design; without one, raise/disable the horizon. |
| `R-ANA-001` | Certified reachable deadlock. Fix dependency/lock structure or policy. |
| `R-ANA-002` | Certified per-task lower-bound WCRT exceeds deadline. Change priorities, cores, or periods. |
| `R-ANA-003` | Periodic local deadlock. Change period, priority, or core assignment. |
| `E-INT-*` | Analyzer/internal failure or unwritable output. Report it; do not treat it as a model verdict. |

Use `code`, `category`, `subject`, and `exit_code` for branching. `message` and
`fix` are informational.

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
change the exit code; `E-INT-` dominates. **Never raise `-m` on a proven
counterexample** (exit 6, or `E-PERF-*` accompanied by a counterexample):
truncation does not invalidate a counterexample found in the explored prefix.
Conversely, an upper-bound `wcrt` exceeding a deadline is not a
counterexample and never produces exit 6.

## Trust boundary

- Estimated values are not certified WCET bounds. Never present an estimate
  (including `estimated by "llm"`) as a hard real-time guarantee; only a
  bound the user supplied as `certified by "<tool>"` carries that claim.
- Report analyzer output mechanically; do not claim schedulability that the
  exit code and `exact`/`truncated` fields do not support.
