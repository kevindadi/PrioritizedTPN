# PTPN diagnostics reference

This page is the authoritative list of diagnostic codes emitted by `ptpn`,
their severity, the command/stage that emits them, and the action expected from
a user or an automated caller. It also defines the JSON envelope, exit-code
priority, and the difference between the **legacy** and **diagnostics**
transports.

The RTSL language itself is documented in [rtsl.md](rtsl.md); the CLI, options
and metrics fields are in [usage.md](usage.md).

## 1. Two output transports

Every command accepts `--diagnostics json` and/or `--diagnostics-file PATH`.
They are **off by default**:

| Transport | How to select | stdout | stderr | Process exit |
|---|---|---|---|---|
| Legacy | no diagnostics option | normal program output | human-readable `ERROR:` / `WARNING:` / `HINT:` lines | `0` success, `1` any failure |
| Engaged | `--diagnostics json` and/or `--diagnostics-file PATH` | only the JSON envelope (when `--diagnostics json`) | all logs (including `[STATS]`) and legacy-style direct prints | the envelope's `exit_code` (category code) |

When engaged, the CLI switches spdlog's default logger to stderr and redirects
`std::cout` to `std::cerr` for the duration of the run. The envelope is printed
after the original stdout buffer is restored, so stdout is a single parseable
JSON document. `--diagnostics-file PATH` writes the same envelope to a file; using
both writes it to both destinations.

CLI11 usage errors are detected before the transport is engaged. They therefore
**bypass the envelope** and use CLI11's own exit codes; verified examples are a
missing required option (`106`), an unknown `--policy` value (`105`), and an
invalid `--diagnostics` value (`105`). Do not interpret these as diagnostic
categories.

## 2. Exit codes

| Exit | Meaning | Participating categories |
|---|---|---|
| `0` | success (or only hints / ordinary warnings) | — |
| `1` | internal failure or CLI usage error in the legacy transport | `internal` (always), plus the fallback for an unclassified nonzero result |
| `2` | syntax / input-form failure | `syntax` (`E-SYN-`) |
| `3` | model validation failure | `validation` (`E-VAL-`) |
| `4` | unsupported model/feature/policy class | `unsupported` (`E-UNS-`) |
| `5` | partial results: truncation without a counterexample | `performance` (`E-PERF-`) |
| `6` | analysis counterexample: deadlock, local deadlock, or `wcrt_lb > deadline` | `analysis` (`R-ANA-`) |

Rules:

- Any `internal` diagnostic returns `1` immediately, regardless of other
  entries.
- `performance` entries participate **even though their default severity is
  `warning`**; every other category participates only with severity `error`.
  A lone `E-PERF-001` therefore exits `5`.
- Hints and ordinary warnings (for example `E-VAL-218` in `check`,
  `E-UNS-005`, `E-VAL-028..032`) do not change the exit code.
- Priority is `internal 1 > syntax 2 > validation 3 > unsupported 4 > analysis
  6 > performance 5 > success 0`. Note that analysis maps to the numerically
  larger `6` but outranks performance.
- The collector keeps at most **10** entries. Once the cap is reached, further
  entries are dropped and `truncated_diagnostics` is `true`. A dropped entry
  cannot influence the exit code.
- In the engaged transport the process exits with the envelope's `exit_code`,
  not with the command's internal return value. If a command returns nonzero
  without a specific diagnostic (or the original entry was dropped), `main`
  adds `E-INT-001` (`operation failed with exit code N`), producing exit `1`.

## 3. JSON envelope

`Diagnostics::to_json()` produces:

```json
{
  "protocol": "ptpn-diagnostics",
  "version": 1,
  "ok": true,
  "exit_code": 0,
  "diagnostics": [
    {
      "code": "E-VAL-206",
      "category": "validation",
      "severity": "error",
      "subject": "task:a:period",
      "location": {"file": "model.rtsl", "line": 3, "column": 20, "span": 3},
      "message": "period must be greater than zero",
      "fix": "set period to a positive time value"
    }
  ],
  "truncated_diagnostics": false,
  "result": {}
}
```

- `protocol` and `version` are `"ptpn-diagnostics"` and `1`.
- `ok` is exactly `exit_code == 0`. Consequently a performance warning makes
  `ok` false while a lone validation warning leaves it true.
- `diagnostics` is always an array (possibly empty).
- `subject` is omitted when absent or empty.
- `location` is omitted when unknown; otherwise only non-empty/nonzero fields
  are serialized. A location with only `file` is valid and is common for file
  open errors and TDG JSON/PTPN validation.
- `fix` is omitted when absent or empty.
- `truncated_diagnostics` is always present.
- `result` is omitted when null or an empty object. It is present for:
  - **successful `check`**: `{"pending_measurements": [...]}`;
  - **analysis runs**: `{"analysis": {...}}` (see §6).

### 3.1 Source locations

RTSL lexer/parser/validator diagnostics carry byte-accurate `line`, `column`
and `span` from the `SourceMap` or the token. Lowering `E-UNS-001` subjects
resolve `task:<id>`, `node:<name>`, `edge:<src>-><tgt>` and `policy:<name>`
through the SourceMap, including implicit fork/join nodes. Analysis-time
`E-UNS-001` resolves `task:<name>` when the task span is available;
`option:extrapolation`/`option:symmetry` have no source position and keep the
location absent. Analysis `R-ANA-*` diagnostics use the metric task name but
have **no** location. TDG JSON validator diagnostics have no location; JSON
parse errors do. PTPN (`E-VAL-10x`) diagnostics carry the file name but zero
line/column.

### 3.2 Caps, taint and cascade suppression

- One collector per stage; `append()` copies entries, applies the receiving
  collector's 10-entry cap, and ORs the truncated flag.
- The RTSL parser marks the failed construct's subject as *tainted*
  (`system:cpus`, `task:<id>`, `task:<id>:time`, ...). Later diagnostics with
  exactly that subject are dropped, which suppresses cascades after a syntax
  error. Taint is exact-string matching, not prefix matching.
- JSON `validate()` removes previous `validation` entries before re-validating;
  RTSL relies on one validation pass per collector.

## 4. Command-level behavior

The table below is the quick reference for the three RTSL-facing commands.

| Situation | `ptpn check` | `ptpn rtsl` | `ptpn allocate` |
|---|---|---|---|
| pending `measured by llm` timing | succeeds; lists tasks in `result.pending_measurements` | rejects with `E-VAL-213`, exit `3` | rejects with `E-VAL-213`, exit `3` |
| `core any` | succeeds with `E-VAL-218` **hint** | rejects with `E-VAL-218` error, exit `3` | expands the domain to `0..num_cpus-1`; no diagnostic |
| `interrupt` task | `E-UNS-005` **hint**, exit `0` | hint, exit `0`; lowered as a non-periodic task with the interrupt marker | hint, exit `0`; same lowering |
| locked task with the wrong segment count | succeeds (lowering-only rule) | `E-VAL-015`, exit `3` | `E-VAL-015`, exit `3` |
| EDF/FIFO/LLF/PIP/PCP/SRP supported-class violation | succeeds (lowering-only rule) | `E-UNS-001`, exit `4` | candidate is treated as infeasible; **no** `E-UNS-001` diagnostic |
| job-clock overlap, or a job-clock + extrapolation/symmetry combination detected during analysis | not run | `E-UNS-001`, exit `4`; legacy exit `1` | not emitted (candidate-level rejection) |
| analysis deadlock / deadline counterexample | not run | `R-ANA-*`, exit `6` | not emitted by the allocation search |
| truncation without counterexample | not run | `E-PERF-*`, exit `5` | reflected in the report's feasibility/truncation fields, not as a diagnostic |

Stage coverage by command:

| Command | Parse/validate | Lowering | Analysis | Codes that can appear |
|---|---|---|---|---|
| `ptpn check` | RTSL | no | no | `E-SYN-002`, `E-SYN-101/102`, `E-UNS-003/004/005`, `E-VAL-201..220` (not `E-VAL-213`), CLI `E-INT-001/002` |
| `ptpn rtsl` | RTSL | yes | yes | `check` codes and `E-VAL-213`, lowering `E-VAL-011/014/015`, `E-UNS-001` (lowering or analysis), `E-INT-003`, `R-ANA-001/002/003`, `E-PERF-001/002`, `E-INT-002` |
| `ptpn allocate` | RTSL or JSON | via TDG2PN per candidate | no protocol diagnostics | input codes, RTSL lowering `E-VAL-011/014/015`; **not** `E-UNS-001`/`E-INT-003`/`R-ANA-*`/`E-PERF-*`; `E-INT-002` for the report |
| `ptpn tdg` | TDG JSON | yes | yes | `E-SYN-001/002`, `E-VAL-001..033`, `E-UNS-001` (lowering or analysis), `E-INT-003`, `R-ANA-*`, `E-PERF-*`, `E-INT-002` |
| `ptpn ptpn` | PTPN source | no | yes | `E-SYN-002`, `E-VAL-101..113`, parser `E-INT-001`, `E-UNS-001` (analysis), `R-ANA-*`, `E-PERF-*`, `E-INT-002` |

**`check` deliberately stops before lowering.** It does not emit
`E-UNS-001`, `E-VAL-011`, or `E-VAL-015`; a successful check means "the source
parses and validates", not "the model is analyzable". Run `ptpn rtsl` to test
the supported class.

`allocate` also accepts TDG JSON. In that case the input codes are the JSON
set below, but allocation still does not emit analysis-phase diagnostics: an
infeasible candidate is recorded in the allocation report.

## 5. Code tables

Severity is the effective severity after per-emission overrides. `error` is
the category default; `warning`, `hint` and `info` are explicit overrides.
“Stage” is the layer that creates the entry, not necessarily the command the
user ran.

### 5.1 Shared, application and analysis codes

| Code | Cat./sev. | Stage / commands | Trigger | Subject / location | Fix |
|---|---|---|---|---|---|
| `E-INT-000` | internal/error | collector fallback | an API caller adds a `Diagnostic` with an empty `code` | as supplied / as supplied | none (library bug) |
| `E-INT-001` | internal/error | PTPN parser catch; CLI catch and post-run fallback | uncaught exception; unknown exception; nonzero result with no retained error | PTPN: none; CLI: `system:internal` / none | `report this as an internal error`; fallback: `report this failure; it has no specific diagnostic` |
| `E-INT-002` | internal/error | output writers and CLI envelope file | a writer fails to open, write, flush or close: TDG DOT, PTPN/SCG DOT, metrics JSON, WCET JSON, TDG JSON, allocation report, or diagnostics file | `output:<path>` / none | `check that the output path is writable`; parent directories are not created automatically |
| `E-INT-003` | internal/error | TDG2PN lowering catch | `TDG2PN::transform` throws something other than `ptpn::UnsupportedClassError` (defensive runtime/logic errors) | `policy:<policy>` / none | `this is an internal lowering failure; report it` |
| `E-UNS-001` | unsupported/error | TDG2PN lowering guard, or the analysis supported-class guard (`rtsl`, `tdg`, `ptpn`) | EDF/FIFO/LLF static-partition rules, PIP/PCP/SRP capacity/start rules, a job-clock overlapping/multiple-in-flight job, or job-clock + extrapolation/symmetry are violated | lowering: `task:<id>`, `node:<fork/join>`, `edge:<src>-><tgt>`, `policy:<policy>`; analysis: `task:<name>`, `option:extrapolation`, `option:symmetry`; RTSL gets a SourceMap span when the subject is locatable | simplify the model or choose a supported policy/option |
| `E-PERF-001` | performance/**warning** | analysis (`tdg`, `rtsl`, `ptpn`, full and cone) | state-class graph reached `--max-states` | `system:max_states` / none | with a counterexample: fix the design, do not raise `-m`; otherwise raising `-m` may help |
| `E-PERF-002` | performance/**warning** | analysis (`tdg`, `rtsl`, `ptpn`, full graph only) | `--horizon` actually pruned expansion | `system:horizon` / none | same as above; otherwise raise/disable `--horizon` |
| `R-ANA-001` | analysis/error | analysis result (`tdg`, `rtsl`, `ptpn`; not cone) | a certified reachable deadlock exists | `system:deadlock` / none | fix dependency/lock structure or policy |
| `R-ANA-002` | analysis/error | analysis result (`tdg`, `rtsl`, `ptpn`) | `wcrt_lb` is finite, the task is observed, and `wcrt_lb > deadline` | `task:<name>` / none | adjust priorities, cores, or periods |
| `R-ANA-003` | analysis/error | analysis result (`tdg`, `rtsl`, `ptpn`) | certified periodic local deadlock | `task:<name>` / none | change period, priority, or core assignment |
| `E-SYN-002` | syntax/error | input open: JSON, PTPN, RTSL, check | input file cannot be opened | none / file only | `check that the input path exists and is readable` |
| `E-SYN-001` | syntax/error | TDG JSON parse | nlohmann parse error | none / precise file:line:column | fix JSON syntax near the reported position |
| `E-VAL-033` | validation/error | TDG JSON parse/type conversion | non-parse exception, usually a field type/shape mismatch | none / file only for file input | check every JSON field's type and shape |

`E-SYN-002` has four message wordings: `Failed to open file:` (JSON),
`Cannot open file:` (PTPN), and `failed to open RTSL file:` (RTSL/check).
They are all syntax-category entries with exit code `2` in the engaged
transport; the legacy `check` transport prints the RTSL wording **without** the
code.

### 5.2 TDG JSON front-end (`tdg`, and `allocate` on JSON)

All entries are emitted by validation unless noted. JSON validator entries have
no location.

| Code | Sev. | Trigger | Subject | Fix |
|---|---|---|---|---|
| `E-VAL-001` | error | `task_place_capacity < 1` | `config:task_place_capacity` | set it to `1` |
| `E-VAL-002` | error | `num_cpus < 1` | `config:num_cpus` | set at least 1 |
| `E-VAL-003` | error | `cores_per_cpu < 1` | `config:cores_per_cpu` | use a positive value (`1` is the supported value) |
| `E-VAL-004` | error | duplicate node id | `node:<id>` | rename/remove the duplicate |
| `E-VAL-005` | error | unknown node type | `node:<id>` | use `task`, `fork`, `join`, or `empty` |
| `E-VAL-006` | error | task core outside `[0,num_cpus-1]` | `task:<id>` | use a valid core |
| `E-VAL-007` | error | `deadline < 0` | `task:<id>` | use a non-negative deadline or omit it |
| `E-VAL-008` | error | empty `allowed_cores` | `task:<id>` | list at least one core or remove the field |
| `E-VAL-009` | error | allowed core outside range | `task:<id>` | list only valid cores |
| `E-VAL-010` | error | duplicate allowed core | `task:<id>` | remove duplicates |
| `E-VAL-011` | error | `core` not in the allowed domain (JSON); defensive re-check in RTSL lowering | `task:<id>` (RTSL lowering gets a task span) | include `core` in `allowed_cores` |
| `E-VAL-012` | error | `wcet_kind` not `model/certified/measured/estimated` | `task:<id>` | use one of the four labels |
| `E-VAL-013` | error | lock not in `configuration.shared_locks` | `lock:<lock>` (not the node) | declare the lock or remove it |
| `E-VAL-014` | error | lock name lacks `mutex`/`spin` prefix | `lock:<lock>`; RTSL gets the lock token span | rename the lock |
| `E-VAL-015` | error | interval count != `2*locks+1` | `task:<id>`; RTSL gets a task span | provide `2*lock_count+1` intervals |
| `E-VAL-016` | error | interval `min > max` | `task:<id>` | ensure `min <= max` |
| `E-VAL-017` | error | fork/join core not `-1` and outside range | `node:<id>` | use `-1` or a valid core |
| `E-VAL-018` | error | edge source unknown | `edge:<src>-><tgt>` | declare the node or fix the endpoint |
| `E-VAL-019` | error | edge target unknown | `edge:<src>-><tgt>` | declare the node or fix the endpoint |
| `E-VAL-020` | error | start binding unknown node | `task:<task>` | reference an existing node |
| `E-VAL-021` | error | start binding not a task | `task:<task>` | reference a `task` node |
| `E-VAL-022` | error | negative start tokens | `task:<task>` | use a non-negative count |
| `E-VAL-023` | error | end binding unknown node | `task:<end>` | reference an existing node |
| `E-VAL-024` | error | end binding not a task | `task:<end>` | reference a `task` node |
| `E-VAL-025` | error | periodic binding unknown node | `task:<task>` | reference an existing node |
| `E-VAL-026` | error | periodic binding not a task | `task:<task>` | reference a `task` node |
| `E-VAL-027` | error | periodic period `<= 0` | `task:<task>` | use a positive period |
| `E-VAL-028` | **warning** | fork/join declares locks (ignored) | `node:<id>` | remove locks from fork/join nodes |
| `E-VAL-029` | **warning** | fork/join has multiple intervals (first used) | `node:<id>` | keep at most one interval |
| `E-VAL-030` | **warning** | start task has predecessor edges | `task:<task>` | remove predecessors or the start binding |
| `E-VAL-031` | **warning** | end task has successor edges | `task:<end>` | remove successors or the end binding |
| `E-VAL-032` | **warning** | graph has nodes but no task | `system:nodes` | add a task node |
| `E-VAL-033` | error | other exception, typically type/shape mismatch | none | check JSON field types |

### 5.3 PTPN source front-end (`ptpn`)

Validation stops at the first failure. Locations are file-only (line/column/span
are zero) even when a file name is present.

| Code | Sev. | Trigger | Subject | Fix |
|---|---|---|---|---|
| `E-VAL-101` | error | empty place id (unreachable through the grammar) | none | give the place an id |
| `E-VAL-102` | error | duplicate place id | `place:<id>` | rename the duplicate |
| `E-VAL-103` | error | negative capacity (unreachable; read unsigned) | `place:<id>` | use a non-negative capacity |
| `E-VAL-104` | error | empty transition id (unreachable) | none | give the transition an id |
| `E-VAL-105` | error | duplicate transition id | `transition:<id>` | rename the duplicate |
| `E-VAL-106` | error | `max < min` (the negative half is unreachable) | `transition:<id>` | write `[min,max]` with `0 <= min <= max` |
| `E-VAL-107` | error | invalid/empty interval, e.g. `(2,2)` | `transition:<id>` | use a non-empty interval |
| `E-VAL-108` | error | arc source undeclared | `arc:<src>-><tgt>` | declare the source |
| `E-VAL-109` | error | arc target undeclared | `arc:<src>-><tgt>` | declare the target |
| `E-VAL-110` | error | place→place or transition→transition arc | `arc:<src>-><tgt>` | connect place↔transition |
| `E-VAL-111` | error | arc weight `<= 0` | `arc:<src>-><tgt>` | use a positive weight |
| `E-VAL-112` | error | `@init` references an undeclared place | `place:<place>` | declare the place |
| `E-VAL-113` | error | negative initial tokens (unreachable) | `place:<place>` | use a non-negative count |

### 5.4 RTSL front-end (`check`, `rtsl`, `allocate` with `.rtsl`)

| Code | Sev. | Stage / commands | Trigger | Subject / location | Fix |
|---|---|---|---|---|---|
| `E-SYN-101` | error | lexer; validator | lexer: unterminated comment/string, invalid escape, invalid/out-of-range integer, illegal character. Validator: unknown `time_unit` or time suffix | lexer: none, precise span. Validator: `system:time_unit` or `task:<id>:{time,period,mit,deadline}`, precise span | close the comment/string, use `\"`, `\\`, `\n`, or use `ms`/`s` |
| `E-SYN-102` | error | panic-mode parser | unexpected token or EOF; suppressed after a lexer error | none; precise token span | rewrite the line with the expected tokens |
| `E-UNS-003` | error | validator | `sporadic` task kind | `task:<id>`; kind/task span | sporadic stays rejected; with the user's agreement you may model a periodic reference scenario with an explicit period such as `period 10ms`. That changes the model and does not certify the original sporadic system |
| `E-UNS-004` | error | validator | `time_unit us` or a `us`-suffixed value | `system:time_unit` or task time sub-attribute; precise span | use `ms` and adjust precision |
| `E-UNS-005` | **hint** | validator | `interrupt` task kind; it is lowered as a non-periodic task with the marker, but preemption is not implemented | `task:<id>`; kind/task span | no action; semantically a hint, exit stays `0` |
| `E-VAL-201` | error | validator | duplicate task id | `task:<id>`; duplicate id token | rename one task |
| `E-VAL-202` | error | validator | `__` in a task id or lock name (reserved for implicit nodes) | `task:<id>` or `lock:<name>`; token span | remove `__` |
| `E-VAL-203` | error | validator | duplicate edge (two entries, one per declaration) | `edge:<sources>-><targets>`; whole-edge spans | remove one declaration |
| `E-VAL-204` | error | validator | edge/binding references an unknown task | `edge:...`, `start:<name>`, or `end:<name>`; endpoint span | add the task or fix the reference |
| `E-VAL-205` | error | validator | `period` on non-periodic, or `mit` on non-sporadic | `task:<id>:period` / `task:<id>:mit` | remove the attribute or change the kind |
| `E-VAL-206` | error | validator | `period <= 0` | `task:<id>:period` | use a positive period |
| `E-VAL-207` | error | validator | `mit <= 0` | `task:<id>:mit` | use a positive mit |
| `E-VAL-208` | error | validator | `deadline <= 0` | `task:<id>:deadline` | use a positive deadline |
| `E-VAL-209` | error | validator | single core or set element outside `[0,cpus-1]` | `task:<id>:core`; core span | use a valid core |
| `E-VAL-210` | error | validator | empty core set or duplicate element | `task:<id>:core`; core span | list distinct cores or use `core N` / `core any` |
| `E-VAL-211` | error | validator | missing `time`; periodic missing `period`; sporadic missing `mit` | `task:<id>`, `task:<id>:period`, or `task:<id>:mit`; task/value span | add the required attribute |
| `E-VAL-212` | error | validator | interval `min > max` after normalization | `task:<id>:time`; interval span | ensure `min <= max` |
| `E-VAL-213` | error | validator (`rtsl`, `allocate`; **not** `check`) | `measured` time has no intervals and mode != Check | `task:<id>:time`; whole `time` span | fill the intervals in the RTSL source |
| `E-VAL-214` | error | validator | `estimated`/`certified` time has no intervals | `task:<id>:time`; whole `time` span | add `[min,max]` intervals |
| `E-VAL-215` | error | validator | unknown policy name | `policy`; value span | use one of the listed policies |
| `E-VAL-216` | error | validator | edge with ≥2 sources and ≥2 targets | canonical `edge:...`; whole edge | split into explicit intermediate tasks |
| `E-VAL-217` | error | validator | missing/`<1` `cpus`; `cores_per_cpu < 1` | `system:cpus` / `system:cores_per_cpu` | add a positive `cpus` / `cores_per_cpu` |
| `E-VAL-218` | **hint in `check`; error in `rtsl`** | validator | task uses `core any`; `allocate` expands it instead | `task:<id>:core`; core spec span | use `ptpn allocate`, or give an explicit core |
| `E-VAL-219` | error | validator | an attribute is declared more than once | `system:*`, `policy`, or `task:<id>:<attr>`; duplicate token span | remove the duplicate declaration |
| `E-VAL-220` | error | validator | measured-block `runs` or `timeout <= 0` | `task:<id>:time`; value span | use a positive integer |
| `E-VAL-011` | error | RTSL lowering | resolved core not in the allowed domain (defensive invariant) | `task:<id>`; task span | include the core in the domain |
| `E-VAL-014` | error | RTSL validator and lowering | lock name lacks `mutex`/`spin` | `lock:<name>`; lock token span | rename the lock |
| `E-VAL-015` | error | RTSL lowering (`rtsl`, `allocate`) | interval count != `2*locks+1` | `task:<id>`; task span | provide `2*lock_count+1` intervals |

### 5.5 Codes that are easy to misread

- **Prefix is not the action.** `E-SYN-002` is file I/O, not grammar;
  `E-VAL-033` is JSON type conversion; `E-SYN-101` is emitted by both the
  lexer (no subject) and the validator (time units, with subject and span).
  `E-VAL-011` and `E-VAL-015` exist in both the JSON validator and RTSL
  lowering with different subject/location conventions.
- **Mode-dependent severity.** `E-VAL-218` is a hint in `check` but an error in
  `rtsl`; `E-UNS-005` is a hint in every command; `E-VAL-028..032` are
  warnings; `E-PERF-*` are warnings that still produce exit `5`.
- **No code for some conditions.** `E-UNS-002` is not defined. `E-VAL-034+`
  and `E-SYN-003+` do not exist. `E-UNS-001/INT-003/R-ANA-*/E-PERF-*` are not
  emitted by `allocate`.
- **Defensive/unreachable branches** include `E-INT-000` (empty-code fallback),
  the lowering branches of `E-VAL-213`/`E-VAL-011`, PTPN `E-VAL-101/103/104/113`,
  and the negative half of `E-VAL-106`. They are kept for library callers.
- **Output-writer coverage is uniform.** Every CLI-visible writer uses
  `common/checked_output.h` and checks open, write, flush and close: TDG DOT,
  TDG JSON, PTPN DOT, SCG DOT, WCET JSON, metrics JSON, the allocation report
  and the diagnostics envelope file. The `/dev/full` regression covers the
  export writers, the allocation report and the diagnostics file. Parent
  directories are never created automatically.
- **Typed supported-class boundary.** `ptpn::UnsupportedClassError` (derives
  from `std::invalid_argument`) is the only type mapped to `E-UNS-001`.
  Lowering guards and the verified analysis guards (job-clock overlapping
  releases, FIFO multiple running jobs, job-clock + extrapolation, applied
  job-clock + symmetry) are typed. Metadata/index/clock-layout inconsistencies
  and programming invariant failures remain `std::invalid_argument` /
  `std::runtime_error` / `std::logic_error` and surface as internal
  (`E-INT-003` in lowering, `E-INT-001` from the CLI top-level handler). Do not
  broaden the mapping to all `invalid_argument`.
- **Known lowering limitation: spin-lock segment marking.** For a single spin
  lock, `vertex_lowering.cpp` indexes `locks` with the execution-segment index,
  producing `suspendable = false, true, true` while the lock's actual critical
  section is the second execution segment. Segment 1 (pre-critical) is marked
  non-suspendable and the critical section is suspendable. This is an existing
  lowering bug; see `docs/rtsl.md` §7/§12.2 and
  `example/diagnostics/spin-lock.rtsl`. A lock-semantics fix is a separate
  follow-up.
- **Allocation swallows candidate exceptions.** `allocation.cpp` catches every
  `std::exception` per candidate, marks it infeasible, and continues; this
  preserves the existing candidate-level rejection protocol, including for the
  typed unsupported class. Unknown internal exceptions are therefore also
  reported only as an infeasible candidate. Separating "expected unsupported"
  from "candidate-internal failure" is a future change with its own scope; it
  is not claimed resolved here.

## 6. Interpreting analysis results

The envelope's `result.analysis` is the machine-readable verdict for
`tdg`/`rtsl`/`ptpn` analysis:

```json
{
  "analysis": {
    "exact": true,
    "truncated": false,
    "max_states": 10000,
    "horizon_truncated": false,
    "states": 26,
    "transitions": 30,
    "deadlock": false,
    "deadlock_states": 0,
    "schedulable": true,
    "local_deadlocks": [],
    "deadline_misses": [],
    "counterexample": null
  }
}
```

- `counterexample` is `"deadlock"`, `"local-deadlock"`, `"deadline"`, or
  `null`. A non-null value is a decisive negative result found in the explored
  prefix and exits `6`.
- `schedulable` is `null` when horizon truncation makes deadlock/SCC analysis
  uncertified; do not read `null` as success.
- `E-PERF-001/002` mean the explored graph is incomplete. If a counterexample
  is already present, raising `-m` cannot change the verdict; fix the design.
  Without a counterexample, raising `-m` may turn a partial result into a
  complete one.
- An **upper-bound** `wcrt` greater than a deadline is not a counterexample and
  does not produce exit `6`. `R-ANA-002` requires the *lower bound*
  (`wcrt_lb`) to exceed the deadline. Upper-bound zone merging makes
  `wcrt_lb` unavailable.
- `exact` is `false` under upper-bound merging, extrapolation, horizon pruning,
  truncation, or symmetry reduction. In that case timing values are bounds,
  not certified exact values. Canonicalization other than `equality` also
  makes the run inexact.

### 6.1 Timing provenance

`wcet_kind` (TDG JSON) or the RTSL `time` declaration records provenance:

| Label | Meaning |
|---|---|
| `certified` | a user/tool-supplied safe bound (for example `certified by "<tool>"`) |
| `measured` | an observed measurement, not a proven WCET bound |
| `estimated` | an informal estimate (for example `estimated by "llm"`) |
| `model` | the default when no provenance is supplied |

Only `certified` carries a hard real-time claim. `measured` and `estimated`
values must not be presented as certified evidence; the WCET export's
`all_certified` flag is true only when every task is `certified`.

## 7. Repair workflows

### 7.1 Pending timing

```bash
./build/ptpn check -f model.rtsl --diagnostics json   # exit 0 + pending_measurements
# fill each listed task:
#   time measured by llm { runs 10, timeout 2 }
# ->
#   time [ [1, 1], [1, 2], [1, 1] ] estimated by "llm"
./build/ptpn check -f model.rtsl --diagnostics json   # pending_measurements == []
./build/ptpn rtsl -f model.rtsl --diagnostics json
```

A locked task needs `2*lock_count+1` intervals. If the count is wrong,
`ptpn check` still succeeds (the rule is in lowering) and `ptpn rtsl` reports
`E-VAL-015`. While any pending task remains, `rtsl`/`allocate` report
`E-VAL-213`.

### 7.2 Open core assignment

- `check`: `E-VAL-218` hint, exit `0`.
- `rtsl`: `E-VAL-218` error, exit `3`.
- `allocate`: expands `core any` to every core and searches; no diagnostic.
- To analyze directly, replace `core any` with an explicit core or core set.

### 7.3 Unsupported class

`E-UNS-001` is emitted by lowering or by the verified analysis guards, not by
validation. Simplify the model or choose a different policy/option; check the
message's subject for the offending `task:`, `node:`, `edge:`,
`option:extrapolation`, or `option:symmetry`. `ptpn check` will not report it
because it does not lower or analyze. `ptpn allocate` keeps candidate-level
rejection: a candidate that raises the typed error is marked infeasible and
the search continues (it does not emit `E-UNS-001` or fail the whole search).

### 7.4 Truncation versus counterexample

- Counterexample present (`counterexample != null`, exit `6`): the result is
  already decisive; raising `-m` is not a fix.
- Only `E-PERF-*` (exit `5`): the result is partial; raise `-m` or adjust
  `--horizon`, then rerun. Deadlock/SCC/schedulability claims are not
  certified while truncated.

### 7.5 Design changes need a diff and user approval

Deleting a lock or dependency, changing the policy, or changing a period or
deadline changes the model's semantics. Automated callers must show the diff
and the before/after analysis, and let the user decide; they must not silently
rewrite the design to make a diagnostic disappear.

## 8. Related documentation

- [rtsl.md](rtsl.md) — RTSL syntax, defaults, units, locks, provenance.
- [usage.md](usage.md) — commands, options, reductions, metrics fields.
- [json_format.md](json_format.md) — TDG JSON input format.
- [architecture.md](architecture.md) — where diagnostics are produced and how
  the envelope is transported.
