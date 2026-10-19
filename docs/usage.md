# Using PTPN

This page describes the CLI subcommands, common options, reductions,
core-allocation search, the LLM integration, scheduling policies, and example
command lines. For build and installation instructions see
[install.md](install.md). The binary is `build/ptpn` in a default native build.

## Input modes

| Mode | Command | Input | Pipeline |
|------|---------|-------|----------|
| **RTSL** | `ptpn rtsl -f model.rtsl` | RTSL v1 source (declarative front-end) | RTSL → AST/validation → TDG (in process) → PTPN → analysis |
| **Check** | `ptpn check -f model.rtsl` | RTSL v1 source | RTSL → AST/validation only (no lowering) |
| **Measurement** | `python -m tools.llm measure --rtsl model.rtsl --tasks-json tasks.json` | RTSL with unmeasured `measured by llm` tasks | `ptpn check` envelope → generated C → measured intervals injected into `.rtsl` |
| **TDG** | `ptpn tdg -f model.json` | Task dependency graph JSON | JSON → TDG → PTPN → analysis |
| **Direct** | `ptpn ptpn -f model.ptpn` | PTPN source file | `.ptpn` → PTPN → analysis |

RTSL time values are normalized to **milliseconds** (for RTSL input the engine
tick is 1 ms). TDG JSON integers remain unit-agnostic engine ticks.

## Subcommands

| Subcommand | Purpose |
|------------|---------|
| `ptpn tdg` | Load TDG JSON, lower to PTPN, analyze and/or export |
| `ptpn rtsl` | Load RTSL source, lower in process to TDG, then run the TDG analysis pipeline |
| `ptpn check` | Lex, parse, and validate RTSL (no lowering or analysis) |
| `ptpn ptpn` | Load `.ptpn` source, analyze and/or export |
| `ptpn allocate` | Search core allocations from TDG JSON or RTSL (`core any` is expanded) |

## Common options (`tdg` / `rtsl` / `ptpn`)

| Option | Description |
|--------|-------------|
| `-f, --file` | Input file (required) |
| `-m, --max-states` | Max states in reachability graph (default: `10000`) |
| `--canonicalization` | `equality` (default), `max-lower`, or `intersection` |
| `--keep-suspension-clocks` | Keep `w_t` in the DBM (default: project it out; exact optimization) |
| `--reduction` | `none` (default), `upper-bound` (sound WCRT upper bound, `wcrt_lb` unavailable), `cone` (per-task cone analysis; no whole-net deadlock), or `symmetry` (verified isomorphic tasks on distinct cores; pooled timing metrics are conservative) |
| `--extrapolation` | Enable opt-in k-extrapolation (timing metrics become approximate) |
| `--horizon N` | WCRT time-horizon reduction: do not expand state classes whose accumulated lower-bound dwell exceeds `N` (default: off). Sound for WCRT only when `N` bounds the response time of interest; deadlock/SCC results are then uncertified |
| `--no-analysis` | Skip state-class reachability analysis |
| `--diagnostics json` | Emit the versioned JSON diagnostics envelope on stdout and reserve stdout for it (available on `check`, `tdg`, `rtsl`, `ptpn`, and `allocate`) |
| `--diagnostics-file PATH` | Write the same envelope to `PATH` |
| `--debug` | Enable debug logging |
| `-v, --version` | Show version |

**Input-lowering options (`tdg` / `rtsl`):**

| Option | Description |
|--------|-------------|
| `--policy` | Select the scheduling policy for lowering and analysis (see [Scheduling Policies](#scheduling-policies)) |

**Exports (all opt-in — paths are exactly what you pass):**

| Option | Output |
|--------|--------|
| `--export-tdg PATH` | TDG Graphviz DOT (`tdg`) or lowered TDG JSON round-trippable through `ptpn tdg` (`rtsl`) |
| `--export-wcet PATH` | Per-task WCET summary JSON (`tdg`/`rtsl`) |
| `--export-ptpn PATH` | PTPN structure Graphviz DOT |
| `--export-scg PATH` | State-class reachability graph DOT |
| `--export-metrics PATH` | Performance / schedulability metrics JSON |

> **Note:** Exports are **opt-in**. Running `ptpn tdg -f input.json` alone
> performs analysis but writes **no** output files unless you pass one or more
> export flags.

## RTSL workflow

RTSL is the primary entry point for humans and LLMs. The model below is the
committed [`example/introduction/demo.rtsl`](../example/introduction/demo.rtsl):
periodic and aperiodic tasks, a lock, a fork/join, and one `measured by llm`
task that is deliberately left unmeasured.

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
    time measured by llm { runs 10, timeout 2 }   // pending until measured
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

```bash
# 1. Parse + validate only (no lowering, no analysis). Exit 0; the envelope's
#    result.pending_measurements lists the unmeasured task.
./build/ptpn check -f example/introduction/demo.rtsl --diagnostics json

# 2. Measure the `measured by llm` task and inject its interval in place.
#    --tasks-json maps each pending task id to a natural-language description.
#    --execute compiles and runs the proposed C; without it this is a dry run
#    that writes nothing.
cat > tasks.json <<'JSON'
{"fusion": "merge one sensor sample and update the shared state"}
JSON
python -m tools.llm measure --rtsl example/introduction/demo.rtsl \
    --tasks-json tasks.json --execute

# 3. Full pipeline: lower in process, analyze, export metrics.
./build/ptpn rtsl -f example/introduction/demo.rtsl \
    --export-tdg /tmp/demo.tdg.json --export-metrics metrics.json

# 4. The exported TDG is ordinary input for the JSON pipeline.
./build/ptpn tdg -f /tmp/demo.tdg.json --no-analysis
```

RTSL time values are normalized to **milliseconds** (for RTSL input the engine
tick is 1 ms). Bare integers use `time_unit` (default `ms`); `ms`/`s` suffixes
are absolute; `us` is rejected as E-UNS-004. TDG JSON integers remain
unit-agnostic engine ticks.

Exit codes: `0` success; `2` syntax; `3` validation; `4` unsupported model
class; `5` truncation/partial results; `6` a certified negative analysis
result (deadlock or `wcrt_lb > deadline`); `1` internal error or CLI usage
error. Without `--diagnostics` / `--diagnostics-file`, the CLI keeps its legacy
streams and `0`/`1` exit codes. Models using `core any` must go through
`ptpn allocate`.

## Reduction modes and defaults

The default is `--reduction none`: exhaustive state-class exploration with
**suspension-clock projection** (on by default), which is exact and does not
change the state count. All other reductions are opt-in because each trades
exactness or scope for speed:

| Mode | Effect on bundled models | Metrics status |
|------|--------------------------|----------------|
| `none` + w-projection | exact baseline; L-Bench 40t: 2718 states, 0.04 s | exact |
| `symmetry` | 4 symmetric tasks: 96 → 19 states; 6 tasks: 792 → 34 states | `exact=false`; per-task WCRT/WCRT_LB preserved, secondary timing conservative |
| `upper-bound` | L-Bench 40t: 2718 → 2713; 100t: 545708 → 545705 (slower: 58.5 s vs 53.1 s) | WCRT upper bound; `wcrt_lb` unavailable |
| `cone` | L-Bench 40t: 40 cones × 2718 = 108720 summed states, 1.66 s vs 0.04 s | per-task timing exact; no whole-net deadlock |
| `horizon H` | L-Bench 40t at H=50: 2718 → 2335 states with unchanged WCRT values | WCRT sound only for H ≥ response time; `wcrt_lb`, deadlock and SCC uncertified |

Symmetry gives the largest reduction but only applies to verified isomorphic
tasks on distinct cores, so it cannot be the default for general models.
Upper-bound merging and horizon pruning are sound but provide little benefit on
the bundled connected DAGs. Cone decomposition is exact per task but multiplies
work when every cone equals the full net. A partial-order/persistent-set mode
was prototyped and **withdrawn**: on L-Bench 100t it reduced the graph from
545708 to 87325 states while lowering some reported WCRTs (for example 5 → 4
and 5 → 3), so it pruned genuinely longest paths and is not sound for WCRT.

## Core-allocation search (`ptpn allocate`)

Tasks may declare an allocation domain:

```json
{"id": "T1", "type": "task", "core": 0, "allowed_cores": [0, 1],
 "deadline": 80, "time": [[2, 3]], "locks": [],
 "wcet_kind": "measured", "wcet_method": "target-board run"}
```

Valid core ids are `0 .. num_cpus-1` (`cores_per_cpu` is a capacity parameter,
not an extra range of core ids). `deadline` is an absolute deadline; `0` or
absent means the implicit deadline (the period).

`ptpn allocate` copies the TDG for each candidate assignment, writes the chosen
cores, lowers with `TDG2PN`, and runs the exact SCG/metrics analysis with
equality canonicalization, suspension-clock projection, and no reduction:

```bash
./build/ptpn allocate -f input.json -o allocation.json \
    --objective max-load --max-candidates 64
```

`allocate` also accepts an RTSL source (`-f model.rtsl`): `core any` is
expanded to the full `0 .. num_cpus-1` domain before the search, so the same
model can be checked (`ptpn check`), rejected for direct analysis
(`ptpn rtsl`), or allocated without hand-editing cores.

The report contains:

- `search`: strategy (`exhaustive`/`heuristic`), whether the assignment domain
  was exhausted, domain size, evaluated/feasible counts, truncation, and
  reason;
- `best`: the highest-ranked feasible candidate (or `null`);
- `assignment`: the winning `{task, core}` list;
- `pareto`: non-dominated feasible candidates on WCRT, utilization, and load;
- `timing_model`: per-task WCET provenance and `all_certified`.

Feasibility requires an untruncated, bounded, deadlock-free, schedulable
analysis. `--require-certified-wcet` restricts `best` to tasks whose
`wcet_kind` is `certified`; measured/estimated/model timing is still analyzed
and reported but cannot win under that gate.

## LLM integration (`tools/llm`)

The **mainline** is the RTSL measurement loop. For models that declare
`time ... measured by llm { ... }`, `ptpn check` reports the unmeasured tasks
in `result.pending_measurements`; the measure command asks an LLM for one
self-contained C program per execution segment, compiles and runs them, and
injects the measured intervals plus a binary/compile-command artifact with a
single span replacement:

```bash
python -m tools.llm measure --rtsl model.rtsl --tasks-json tasks.json \
    --execute
```

`--tasks-json` maps each pending task id to a natural-language description;
`--response-file` replays a response offline; `--out` writes elsewhere;
without `--execute` the command is a dry run and writes nothing. Integers are
converted to the model's millisecond base by rounding up (any non-zero
observation is at least 1 ms).

**Trust boundary:** the LLM is an untrusted proposer. Its output is schema
validated; generated programs run only with `--execute`, with a timeout and
best-effort resource limits. The measured min/max wall-clock interval includes
process startup, has no confidence bound, and is **not a WCET upper bound**.
Do not use `wcet_kind=measured` for hard real-time claims; certified timing
requires a sound static WCET tool or validated MBPTA. Locked tasks require
`2 * lock_count + 1` per-segment measurements; the emitter rejects a single
total interval for them.

### Standalone generation prototype

The older prototype turns a validated task specification into LLM-proposed C
programs, compiles and runs them in a temporary directory, and emits a TDG
with `wcet_kind: "measured"`:

```bash
python -m tools.llm generate --spec spec.json --out model.json \
    --execute --runs 20 --timeout 5 --tdg-out graph.json
```

`--response-file` replays a saved model response without network access.
`--dry-run` prints the deterministic prompt. The same trust boundary applies.

## Symmetry reduction (`--reduction symmetry`)

The reduction quotients the state-class graph by permutations of **provably
isomorphic tasks**. It is conservative and off by default.

Candidate tasks must have identical signatures: chain node kinds and
attributes, execution intervals, priority, core behaviour, lock names,
period/deadline, periodic markers, and the external arcs attached to the chain
(consumers, core/lock resources, dependency edges and the neighbouring task
structure). Each candidate swap is then **verified as an automorphism of the
whole net** before use: pre/post arcs and weights, place
capacity/saturation/initial tokens, transition interval/priority/suspension,
periodic transitions fixed, and a consistent relabelling of real cores with
equal parallelism. The control core never moves.

Supported scope (deliberately narrower than full task symmetry):

- Only tasks bound to **distinct real cores** (`core >= 0`); at most one member
  per core. Control-core tasks and tasks sharing a core are never permuted,
  because on a shared core the canonical representative would hide which task
  runs and per-task timing would no longer be recoverable.
- A signature group is dropped when any of its lock names is also used by a
  **non-isomorphic** task outside the group.
- Periodic release transitions must stay fixed, so periodic tasks with distinct
  release transitions are not permuted.

Canonicalization applies to every identity-bearing field of a state class:
marking, clock variables and the derived per-transition clock maps, enabled /
priority / suspended transition sets, the DBM rows and columns, and the
lexicographic state key. The lexicographically smallest representative of the
orbit is used, and the reduction is active only when the verified group is
non-trivial. `elapsed_time` and the graph-assigned `id` are metadata and are
not permuted.

Because a quotient state represents several concrete schedules, timing metrics
are reported as **conservative bounds** (`exact=false` in the metrics JSON).
Per-task values are pooled within each verified task group; on a correctly
symmetric model they reproduce the unreduced per-task WCRT and WCRT lower
bound. Heterogeneous tasks are never merged.

```bash
# 6 identical tasks on 6 distinct cores: 792 states -> 34 states
./build/ptpn tdg -f example/symmetry/symmetric-6t-6c.json \
  -m 10000000 --reduction symmetry --export-metrics metrics.json
```

## Scheduling policies

JSON `configuration.policy` and `--policy` accept values such as:

| Policy | Native analysis status |
|--------|------------------------|
| `fixed` | Default fixed-priority lowering (resume-style native analysis) |
| `fixed_prior_with_resume` | Engine-native preemption (recommended for analysis) |
| `fixed_prior_with_restart` | Structural restart preemption |
| `rm` | Rate-monotonic priorities computed at lowering; resume-style |
| `dm` | Deadline-monotonic priorities computed at lowering; resume-style |
| `pip` | Priority Inheritance Protocol, derived from the marking; resume-style |
| `pcp` | Priority Ceiling Protocol: static lock ceilings, ceiling-based admission and transitive inheritance; resume-style |
| `srp` | Stack Resource Policy: static lock ceilings, global system-ceiling start gate, no inheritance; resume-style |
| `edf` | Earliest Deadline First using per-job release clocks and per-core deadline-order zone branches; statically partitioned multi-core |
| `fifo` | Non-preemptive FIFO using per-job release clocks and per-core arrival-order branches; statically partitioned multi-core |
| `llf` | **Event-driven LLF** using laxity clocks; **not** classical continuous-time LLF; statically partitioned multi-core, deterministic segments only |

- **Native P-TPN analysis:** all policies above use the native firing-rule
  template. PIP/PCP/SRP are
  supported on TDG2PN-lowered nets with `task_place_capacity == 1` and one
  in-flight job per task; PCP/SRP use a global system ceiling (the classical
  protocols are uniprocessor).
- **EDF/FIFO/LLF** support statically partitioned multi-core: tasks stay on
  their declared cores, `cores_per_cpu == 1`, one in-flight job per task, and
  no locks or dependency-driven releases. EDF and FIFO use release clocks; LLF
  requires deterministic execution segments and is event-driven, not classical
  LLF. Unsupported models are rejected explicitly.

## Examples

```bash
# TDG: analyze + export state-class graph and PTPN DOT
./build/ptpn tdg -f example/l-bench/pipeline-40t-8c-5l.json \
  --export-scg scg.dot --export-ptpn ptpn.dot -m 5000

# TDG: larger benchmark (100 tasks, 20 cores, fork/join)
./build/ptpn tdg -f example/l-bench/pipeline-100t-20c.json -m 10000

# TDG: WCET summary only (no reachability)
./build/ptpn tdg -f example/p-bench/initial.json \
  --no-analysis --export-wcet wcet.json

# TDG: statically partitioned multi-core EDF/FIFO/LLF
# (supported-class models: no locks/dependency releases, one job per task)
./build/ptpn tdg -f input.json --policy edf --export-metrics edf.json
./build/ptpn tdg -f input.json --policy fifo --export-metrics fifo.json
./build/ptpn tdg -f input.json --policy llf --export-metrics llf.json

# PTPN: max-lower canonicalization + metrics
./build/ptpn ptpn -f model.ptpn \
  --canonicalization max-lower --export-metrics metrics.json

# RTSL: validate, then run the full pipeline
./build/ptpn check -f model.rtsl --diagnostics json
./build/ptpn rtsl -f model.rtsl --export-tdg lowered.json --export-metrics metrics.json

# l-bench: scaling experiments (example/run.py finds build/ptpn via .ptpn-build-dir)
python3 example/run.py --lbench-cases pipeline-40t-8c-5l -m 5000000 --extrapolation
python3 example/run.py --reviewer-case -m 20000
python3 example/run.py --skip-suites --validate-tdg-only

# p/s/t-bench: export PTPN/SCG DOT
python3 example/run.py
python3 example/run.py --suites p-bench t-bench --profiles ptpn
```

## Example inputs

| Directory | Contents |
|-----------|----------|
| [example/introduction/](../example/introduction/) | Small introductory RTSL model (`demo.rtsl`: periodic + aperiodic, lock, fork/join, pending measurement) |
| [example/motivating-examples/](../example/motivating-examples/) | Motivating TDG examples |
| [example/p-bench/](../example/p-bench/) | Priority / preemption benchmarks |
| [example/s-bench/](../example/s-bench/) | Small scheduling benchmarks |
| [example/t-bench/](../example/t-bench/) | Scalability benchmarks (`d-*.json`) |
| [example/l-bench/](../example/l-bench/) | Progressive pipeline benchmarks (`pipeline-*t-*c.json`, 10–100 tasks, fork/join, scaled shared mutexes; `pipeline-40t-8c-5l.json` for the 5-lock reviewer case) |
| [example/symmetry/](../example/symmetry/) | Symmetric task sets for `--reduction symmetry` |

TDG JSON format: [json_format.md](json_format.md).
