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
  `measured by llm` in `result.pending_measurements`; `ptpn fill` writes the
  measured intervals back into the `.rtsl` source (validating the patched
  model before it is written)
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
- **Resource budgets**: `--timeout` and `--max-memory` stop a runaway
  exploration with partial results and a performance diagnostic (exit 5)
- **Model semantics in plain text**: `ptpn describe` restates the effective
  policy, computed priorities, core mapping, lock nesting, release phases and
  edge forms, so a human or an LLM can check its understanding before trusting
  a verdict
- **Self-describing diagnostics**: `ptpn explain [CODE]` serves the catalog
  generated from `docs/diagnostics.md`
- **MCP server**: `ptpn mcp` exposes check/analyze/describe/fill/explain as
  tools over stdio JSON-RPC for LLM clients

## Webapp

A zero-dependency local web UI lives in [`webapp/`](webapp/README.md). It wraps
`build/ptpn` in a small Python HTTP server and ships two front-ends:

### Story (default at `/`)

A guided, animated explanation of the method:

- why WCET is not a schedule, using the `hw` fixed-priority example;
- TDG → PTPN lowering mapped to places, tokens, timed transitions and priorities;
- a step-through replay of the state-class graph: time elapse, firing,
  E<sub>struct</sub> / E<sub>pri</sub> / suspended sets, DBM clocks and the
  preemption of F by B;
- a policy comparison on the same model (fixed, RM, DM, structural restart,
  EDF, FIFO), including the analyzer's supported-class rejections;
- the LLM workflow: RTSL / TDG JSON / PTPN front doors, pending
  `measured by llm` timing, `check` / `fill` / analyze, and a repair prompt built
  from the analyzer's verdict and witness.

### Workbench (at `/workbench`)

![PTPN Chronograph workbench](webapp/screenshot-overview.png)

The original analysis workbench:

- an RTSL/TDG editor with validation, line diagnostics, and pending
  `measured by llm` measurement filling;
- interactive TDG, PTPN, and state-class graphs (pan/zoom, inspectors);
- WCRT/WCET envelopes, per-core utilization, a response-ratio dial, lock
  contention, and the full metrics table;
- a bottom run log with the analyzer's `[STATS]` timing lines.

```bash
./example/build.py                    # build the analyzer first
python3 webapp/server.py              # story: http://127.0.0.1:8765/
                                      # workbench: http://127.0.0.1:8765/workbench
python3 webapp/server.py --demo       # precomputed showcase (no live API needed)
python3 webapp/make_demo.py           # regenerate webapp/demo/demo-data.js
python3 webapp/build_standalone.py    # single-file workbench demo
python3 webapp/build_standalone.py --story  # single-file story demo
```

The front-end is plain HTML/CSS/JS with no build step. The same pages are used
for the local live server and the precomputed demo; `--demo` swaps in
`webapp/demo/demo-data.js`.

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
| [docs/rtsl.md](docs/rtsl.md) | RTSL v1 reference: lexical rules, system/task syntax, units, cores, locks, provenance, bindings, supported classes |
| [docs/rtsl.ebnf](docs/rtsl.ebnf) | Complete RTSL v1 grammar (EBNF, keywords checked against the lexer) |
| [docs/diagnostics.md](docs/diagnostics.md) | Authoritative diagnostic code table, severity/exit codes, JSON envelope, command behavior, result interpretation |
| [docs/architecture.md](docs/architecture.md) | Pipeline layers, source/CMake layout, application-layer responsibilities, known structural notes |
| [docs/json_format.md](docs/json_format.md) | TDG JSON input format |

## License

See [LICENSE](LICENSE).
