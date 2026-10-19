# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

PTPN is a **Priority Timed Petri Net** analyzer for real-time scheduling. It lowers task dependency graphs into timed Petri nets with priorities, preemption, and locks, then explores state-class reachability graphs (DBM clock zones) for WCRT/schedulability analysis and Graphviz DOT export. Inputs are RTSL (`.rtsl`), TDG JSON, and PTPN source (`.ptpn`). C++17, CMake ≥ 3.27 + Ninja, Boost (`graph`, `filesystem`, `thread`, `date_time`). Other deps (nlohmann/json, CLI11, spdlog/fmt, GoogleTest) are pinned via FetchContent.

## Commands

### Build

```bash
./example/build.py                 # bootstrap vcpkg under ~/vcpkg if missing, configure + build, write .ptpn-build-dir
./example/build.py --test          # build + ctest
./example/build.py --build-dir build-asan --build-type Debug --fresh --clean
./example/build.py --skip-vcpkg    # use system Boost (this is what CI does on Linux)
```

Manual CMake (CI path, system Boost): `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`. vcpkg auto-detection is off on Linux/macOS by default (`-DPTPN_USE_VCPKG=ON` re-enables); on Apple Silicon pass `-DVCPKG_TARGET_TRIPLET=arm64-osx`.

`example/run.py` drives the example benchmarks (p/s/t-bench + l-bench) and resolves the `ptpn` binary via `--ptpn PATH` → `$PTPN_BUILD_DIR` → `.ptpn-build-dir` marker → `build/ptpn` (the marker and default `build/` live at the repo root, not under `example/`). `compile_commands.json` at the repo root is a symlink to the configured build directory's file (currently `build/`).

Binaries: `build/ptpn` (analyzer CLI), `build/test/ptpn_test` (unit tests).

### RTSL commands

```bash
./build/ptpn check -f model.rtsl --diagnostics json      # lex/parse/validate only (no lowering)
./build/ptpn rtsl -f model.rtsl [--export-tdg PATH]      # full pipeline; --export-tdg writes JSON
```

The end-to-end workflow (clarify -> write -> check -> fill -> analyze -> report) and the per-code action table live in `.claude/skills/rt-system/SKILL.md` (repo-local; not built or tested).

### Tests

```bash
ctest --test-dir build --output-on-failure
./build/test/ptpn_test                                   # one monolithic gtest binary
./build/test/ptpn_test --gtest_filter='Symmetry*'        # single test/suite — run from repo root
```

Tests are discovered with `gtest_discover_tests` and `WORKING_DIRECTORY` = repo root, so tests reference `example/...` and `test/golden/...` relative to root (and the binary must be run from the repo root).

Golden tests (`test/test_pipeline_golden.cpp`) diff export output against `test/golden/*`; the header comment documents the exact commands that regenerate each golden file. Golden comparison is skipped when not compiled with libstdc++ (`__GLIBCXX__` guard) — regenerate goldens only on libstdc++ builds.

## Architecture

The pipeline is `parse → model → lowering → analysis`, mirrored one-to-one by the static-library layering in `CMakeLists.txt` (each target declares only the deps it uses; `PTPN_core` is the INTERFACE aggregate for tests). The CLI is its own layer and depends one-way on the application layer:

```
JSON TDG ─parse(ptpn_json)→ TDG(ptpn_tdg) ──tdg2pn(ptpn_lowering)──► PTPN(ptpn_petri) ──► analysis
.rtsl ─parse/validate/lower(ptpn_dsl)──► TDG (in process) ─────────────────────────────────┘
.ptpn source ─parse(ptpn_parser)────────────────────────────────────► PTPN ────────────► analysis
analysis = ptpn_dbm → ptpn_reachability → ptpn_metrics, + ptpn_reductions (cone), ptpn_allocation
CLI = ptpn_cli (src/cli, CLI11 wiring) → {ptpn_app_pipelines, ptpn_app_check, ptpn_app_allocation} → core libraries
```

The application layer is split so commands only pull what they need:
`ptpn_app_support` (options, legacy rendering, memory, RTSL/TDG loading),
`ptpn_app_check` (parse+validate only), `ptpn_app_pipelines` (lowering
adaptation, analysis orchestration, WCET export, pipeline entry points), and
`ptpn_app_allocation` (search driver). `ptpn check` must not link lowering,
analysis, or allocation.

**RTSL front-end (`src/dsl/`):** `.rtsl` → lexer → panic-mode parser → semantic validation → in-process lowering to `model::TDG` → the shared `tdg2pn → analysis` path. No JSON text round-trip. `dsl::SourceMap` is a side table (structured subject → byte-accurate span) used to backfill `location` in diagnostics without polluting the TDG model. `ptpn check` stops after validation; lowering support-class guards (`E-UNS-001`) and missing measured intervals (`E-VAL-213`) are reported by `ptpn rtsl`/`ptpn allocate`. Pending `measured by llm` tasks are listed in `result.pending_measurements`; the user or Claude fills their `time` attribute directly in the `.rtsl` source.

### State-class analysis core (`src/analysis/`)

A `StateClass` (`reachability/state_class.h`) is `(marking, DBM zone)` plus scheduler-facing sets: `struct_enabled` (E_struct), `priority_enabled` (E_pri-hat, the selected active set), `suspended`. The DBM (`dbm/dbm.h`) has one column per clock: `x0`, execution clocks `h_t`, suspension clocks `w_t`, and per-job release `r_i` / laxity `chi_i` clocks (the latter only for EDF/FIFO and LLF respectively). The canonical clock layout is built by `build_clock_layout()`. **Suspension-clock projection is on by default and exact** — w_t columns are dropped from the DBM.

`StateClassReachabilityGraph` (`reachability/ptpn_analysis.h`) drives exploration: `time_elapse`, `fire`, canonicalization modes (`equality`/`max-lower`/`intersection`), `ReductionMode` (NONE / UPPER_BOUND zone-inclusion merging — sound WCRT upper bound, `wcrt_lb` becomes unavailable), opt-in k-extrapolation, `--horizon` dwell-bound pruning, and symmetry reduction (quotient by verified net automorphisms; `exact=false` metrics). Extrapolation and symmetry are rejected for job-clock layouts (EDF/FIFO/LLF) — only sound for transition-indexed layouts of static/PIP/PCP/SRP rules. `FiringEdge::dwell_[min,max]` is the per-edge time weight all timing metrics build on.

### Scheduling policies (`reachability/scheduling_policy.h`)

A `FiringRule` hierarchy computes the admissible selected active set per state class. The state-class graph **branches on scheduling decisions**: each admissible selected active set (and each tied ordering for EDF/FIFO/LLF) yields one branch — not per interleaving. Rules:

- `StaticPriorityRule` — `fixed`, `fixed_prior_with_resume`, `fixed_prior_with_restart` (structural restart preemption), `rm`, `dm`, `pip`, `pcp`, `srp` (PIP/PCP/SRP require `task_place_capacity == 1`, one in-flight job; PCP/SRP use a global system ceiling)
- `EdfFiringRule` / `FifoFiringRule` — per-job release clocks; statically partitioned multi-core
- `LlfFiringRule` — event-driven LLF (laxity clocks), **not** classical continuous-time LLF; deterministic execution segments only

EDF/FIFO/LLF have an explicit **supported class**: static core partition, `cores_per_cpu == 1`, one in-flight job per task, no locks, no dependency-driven releases. Unsupported models are rejected explicitly (enforced in lowering and re-checked when the rule indexes the net).

### Models and CLI

- `src/model/tdg.h` — TDG (tasks, fork/join, locks, dependencies, periodic release, `allowed_cores`, WCET provenance); format in `docs/json_format.md`
- `src/model/petri.h` — PTPN: `Transition` carries `TimeInterval`, `priority`, `core` (−1 = control), `suspendable`; `Marking` is `vector<int>`
- `src/dsl/` — RTSL lexer/parser/validator/lowering and `SourceMap`; the syntax reference is `docs/rtsl.md`, the workflow is in `docs/usage.md`, and `example/introduction/demo.rtsl` is the introductory model
- `src/cli/` — CLI11 layer (namespace `cli`): `main.cpp` (envelope scaffolding + dispatch), `command.h`, `option_groups.{h,cpp}`, and one file per subcommand under `commands/` (`tdg`, `rtsl`, `check`, `ptpn`, `allocate`)
- `src/app/` — application layer (namespace `app`, no CLI11): `common` (options, legacy rendering, memory, timing/log helpers), `input` (RTSL + TDG loading), `check_cmd` (parse+validate only), `lowering` (TDG→PTPN adapter), `analysis` (orchestration, result diagnostics, WCET export), `pipelines` (the `run_*_pipeline` entry points, public `pipelines.h`), and `allocation_cmd`.

User-facing docs live in `docs/` (`install.md`, `usage.md`, `rtsl.md`, `diagnostics.md`, `architecture.md`, `json_format.md`). The diagnostics code table is authoritative in `docs/diagnostics.md`; keep new codes and behavior changes in sync with it and with the rt-system skill.

## Conventions and gotchas

- **`example/run.py` regex-parses `[STATS]` log lines** emitted by the analyzer (SCG build stats, lowering ms, etc.). Changing the text of those log lines breaks `run.py` silently — update `RE_*` patterns in `example/run.py` together with any format change.
- **Diagnostics are opt-in:** `--diagnostics json` / `--diagnostics-file PATH` switch `tdg`/`rtsl`/`check`/`ptpn`/`allocate` to the versioned envelope with exit codes `2/3/4/5/6` and move logs (including `[STATS]`) to stderr. Without them the legacy streams and `0`/`1` exit codes apply. The RTSL repair loop depends on the engaged form. The authoritative code table, envelope schema, and `check`/`rtsl`/`allocate` behavior matrix are in `docs/diagnostics.md`; `ptpn check` stops before lowering by design.
- **Supported-class rejection is typed:** throw `ptpn::UnsupportedClassError` (`src/common/unsupported_class.h`, optional structured subject); the app maps only that type to `E-UNS-001`. Do not classify all `std::invalid_argument` as unsupported — metadata/index/clock-layout invariants stay internal (`E-INT-003` in lowering, `E-INT-001` from the CLI top-level handler). `E-UNS-001` can come from lowering or the verified analysis guards (job-clock overlap, job-clock + extrapolation/symmetry).
- **Output writers must check open, write, flush and close**, and only log success after the check. Failures are `E-INT-002` with subject `output:<path>`; parent directories are never created automatically.
- **RTSL time is milliseconds:** RTSL inputs are normalized so one engine tick = 1 ms (bare integers follow `time_unit`; `us` is rejected). TDG JSON integers remain unit-agnostic engine ticks.
- Metrics JSON carries an `exact` flag: `false` under symmetry reduction, extrapolation, or horizon truncation, and `wcrt_lb` is omitted where the analysis is only an upper bound. Preserve that contract when touching metrics code.
- Tests map one `test_<concern>.cpp` per concern; policy tests exist for both single- and multi-core (`test_edf.cpp` + `test_edf_multicore.cpp`, etc.). Reuse `tdg_test_helpers.h` (`load_tdg`) rather than hand-parsing JSON in tests.
- Commit style is conventional commits (`feat(scheduling): ...`, `docs: ...`).
- `.gitignore` excludes `*.json`/`*.dot`/`*.ppn`/`*.ptpn` broadly; `test/golden/**` is explicitly re-included — keep golden fixtures committed.
- Namespaces are lowercase (`state_class`, `petri`, `app`, `cli`, `dsl`, `converter`); analysis code lives in `namespace state_class`.
- There is no `.clang-format` config and no CI format gate (both were removed deliberately). Match the surrounding code style when editing.
- `example/` holds `build.py`/`run.py` plus all example models; `docs/Dockerfile` builds with apt-installed system Boost (no vcpkg).
