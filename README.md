# PTPN

**PTPN** (Priority Timed Petri Net) is a real-time scheduling analyzer. It
lowers task dependency graphs into timed Petri nets with priorities,
preemption, and locks, then explores their **state-class reachability graphs**
(DBM clock zones) to compute worst-case response times (WCRT) and
schedulability results under explicit scheduling policies.

Models can be written in **RTSL** — a unit-aware, declarative language for
real-time task systems — as **TDG JSON**, or directly as **PTPN source**
(`.ptpn`). RTSL and TDG inputs are lowered to a PTPN in process; the analysis
engine then explores the timed state space. Tasks declared `measured by llm`
remain pending until their `time` attribute is filled in directly in the RTSL
source.

## Features

- **RTSL v1 front-end** (`.rtsl`): unit-aware, declarative source with
  panic-mode diagnostics and in-process lowering to TDG; validation-only and
  full-pipeline modes
- **Pending timing workflow**: `ptpn check` lists tasks declared
  `measured by llm` in `result.pending_measurements`; their `time` attribute is
  filled in directly in the `.rtsl` source
- Parse and validate **TDG JSON** (tasks, fork/join, locks, dependencies,
  periodic release)
- Lower TDGs into **PTPNs** (multi-core fixed/dynamic-priority scheduling,
  preemption, mutex/spin locks)
- **Scheduling policies**: fixed/restart/resume, RM/DM, PIP/PCP/SRP, EDF, FIFO,
  and event-driven LLF; EDF/FIFO/LLF support statically partitioned multi-core
  with explicit supported-class guards
- Parse the **PTPN domain language** (`.ptpn`) directly, skipping TDG conversion
- **State-class reachability analysis** with canonicalization (`equality`,
  `max-lower`, `intersection`)
- **Sound upper-bound zone merging**: one-sided zone-inclusion merging for
  smaller graphs; WCRT stays a certified upper bound, `wcrt_lb` is reported as
  unavailable
- Optional **per-task cone decomposition**: analyze each task on its
  dependency/contention cone; per-task timing and local-deadlock metrics match
  the full graph, but whole-net deadlock detection is unavailable and connected
  DAGs may not shrink
- Optional **WCRT time-horizon reduction** that bounds SCG expansion and
  reports truncation/approximation explicitly
- Conservative **symmetry reduction** for verified isomorphic tasks on distinct
  cores
- **Core-allocation search** over per-task `allowed_cores`, evaluating
  candidates with the exact state-class analysis and reporting feasible/Pareto
  assignments with an optimality status
- **Timing provenance**: estimated values (`estimated by "llm"`) are explicit
  in the model and are explicitly not certified WCET bounds
- Task `deadline` and WCET provenance (`wcet_kind`, `wcet_method`,
  `wcet_artifact`) in TDG JSON
- **Opt-in exports**: TDG / PTPN / state-class Graphviz DOT, WCET JSON, and
  metrics JSON

## Pipeline

```text
RTSL source (.rtsl) ──► lex / parse / validate ──► TDG (in memory) ─┐
TDG JSON            ──► parse ───────────────────► TDG ─────────────┴──► tdg2pn ──► PTPN ──┐
PTPN source (.ptpn) ──► parse ─────────────────────────────────────► PTPN ────────────────┤
                                                                                          ▼
                                        state-class reachability analysis (DBM clock zones,
                                        priorities, preemption, scheduling policies)
                                                                                          │
                                                                                          ▼
                                               WCRT / schedulability / metrics JSON
```

RTSL time values are normalized to milliseconds (one engine tick = 1 ms for
RTSL input); TDG JSON integers remain unit-agnostic engine ticks.

## Documentation

| Document | Contents |
|----------|----------|
| [docs/install.md](docs/install.md) | Dependencies, native build, vcpkg, tests, and the Docker image |
| [docs/usage.md](docs/usage.md) | Subcommands, options, reductions, core allocation, RTSL workflow, examples |
| [docs/json_format.md](docs/json_format.md) | TDG JSON input format |

## License

See [LICENSE](LICENSE).
