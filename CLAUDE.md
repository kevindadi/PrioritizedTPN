# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

PTPN is a **Priority Timed Petri Net** analyzer for real-time scheduling. It lowers task dependency graphs (TDG JSON) into timed Petri nets with priorities, preemption, and locks, then explores state-class reachability graphs (DBM clock zones) for WCRT/schedulability analysis and format export (Romeo CTS, PToPNer `.ppn`, Graphviz DOT). C++17, CMake ≥ 3.27 + Ninja, Boost (`graph`, `filesystem`, `thread`, `date_time`). Other deps (nlohmann/json, CLI11, spdlog/fmt, GoogleTest) are pinned via FetchContent.

## Commands

### Build

```bash
./build.py                 # bootstrap vcpkg under ~/vcpkg if missing, configure + build, write .ptpn-build-dir
./build.py --test          # build + ctest
./build.py --build-dir build-asan --build-type Debug --fresh --clean
./build.py --skip-vcpkg    # use system Boost (this is what CI does on Linux)
```

Manual CMake (CI path, system Boost): `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`. vcpkg auto-detection is off on Linux/macOS by default (`-DPTPN_USE_VCPKG=ON` re-enables); on Apple Silicon pass `-DVCPKG_TARGET_TRIPLET=arm64-osx`.

`run.py` resolves the `ptpn` binary via `--ptpn PATH` → `$PTPN_BUILD_DIR` → `.ptpn-build-dir` marker → `build/ptpn`. The repo currently has an IDE build in `cmake-build-debug/` (root `compile_commands.json` symlinks into it).

Binaries: `build/ptpn` (analyzer CLI), `build/test/ptpn_test` (unit tests).

### RTSL / LLM commands

```bash
./build/ptpn check -f model.rtsl --diagnostics json      # lex/parse/validate only (no lowering)
./build/ptpn rtsl -f model.rtsl [--export-tdg PATH]      # full pipeline; --export-tdg writes JSON
python -m tools.llm measure --rtsl model.rtsl --tasks-json tasks.json [--execute]
```

The end-to-end workflow (generate → check → fix → measure → analyze → report) and the per-code action table live in `.claude/skills/rt-system/SKILL.md` (repo-local; not built or tested).

### Tests

```bash
ctest --test-dir build --output-on-failure
./build/test/ptpn_test                                   # one monolithic gtest binary
./build/test/ptpn_test --gtest_filter='Symmetry*'        # single test/suite — run from repo root
```

Tests are discovered with `gtest_discover_tests` and `WORKING_DIRECTORY` = repo root, so tests reference `example/...` and `test/golden/...` relative to root (and the binary must be run from the repo root).

Golden tests (`test/test_pipeline_golden.cpp`) diff export output against `test/golden/*`; the header comment documents the exact commands that regenerate each golden file. Golden comparison is skipped when not compiled with libstdc++ (`__GLIBCXX__` guard) — regenerate goldens only on libstdc++ builds.

Python tests for the LLM prototype: `python -m unittest discover tools/llm/tests` (stdlib `unittest`, run from repo root).

### Format

clang-format **21.1.8 is pinned by CI** (pip-installed); use that exact version or the check will differ. Style: Google-based, `ColumnLimit: 100`, `PointerAlignment: Left` (see `.clang-format`).

```bash
find src test -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
  | xargs -0 clang-format --dry-run --Werror
```

## Architecture

The pipeline is `parse → model → lowering → analysis → export`, mirrored one-to-one by the static-library layering in `CMakeLists.txt` (each target declares only the deps it uses; `PTPN_core` is the INTERFACE aggregate for executables/tests):

```
JSON TDG ─parse(ptpn_json)→ TDG(ptpn_tdg) ──tdg2pn(ptpn_lowering)──► PTPN(ptpn_petri) ──► analysis
                │                        ├──tdg2romeo(ptpn_romeo)──► Romeo .cts
                │                        └──tdg2ptopner(ptpn_ptopner)► .ppn (requires fixed_prior_with_restart)
.ptpn source ─parse(ptpn_parser)─────────► PTPN ───────────────────► analysis
analysis = ptpn_dbm → ptpn_reachability → ptpn_metrics, + ptpn_reductions (cone), ptpn_allocation
```

`src/lower/convert/` is a facade (`ptpn_convert`) routing TDG → {PTPN, Romeo, PToPNer} with PTPN as the canonical hub.

**RTSL front-end (`src/dsl/`):** `.rtsl` → lexer → panic-mode parser → semantic validation → in-process lowering to `model::TDG` → the shared `tdg2pn → analysis` path. No JSON text round-trip. `dsl::SourceMap` is a side table (structured subject → byte-accurate span) used to backfill `location` in diagnostics without polluting the TDG model. `ptpn check` stops after validation; lowering support-class guards (`E-UNS-001`) and missing measured intervals (`E-VAL-213`) are reported by `ptpn rtsl`/`ptpn allocate`. `tools/llm measure` consumes the `check` envelope's `result.pending_measurements` and rewrites the `time` attribute in the `.rtsl` source. Diagnostics are opt-in: `--diagnostics json` / `--diagnostics-file PATH` route stdout to the versioned envelope and switch to the `2/3/4/5/6` exit-code mapping (protocol in `docs/diagnostics.md`); without them streams and `0`/`1` exit codes stay legacy.

### State-class analysis core (`src/analysis/`)

A `StateClass` (`reachability/state_class.h`) is `(marking, DBM zone)` plus scheduler-facing sets: `struct_enabled` (E_struct), `priority_enabled` (E_pri-hat, the selected active set), `suspended`. The DBM (`dbm/dbm.h`) has one column per clock: `x0`, execution clocks `h_t`, suspension clocks `w_t`, and per-job release `r_i` / laxity `chi_i` clocks (the latter only for EDF/FIFO and LLF respectively). The canonical clock layout is built by `build_clock_layout()`. **Suspension-clock projection is on by default and exact** — w_t columns are dropped from the DBM.

`StateClassReachabilityGraph` (`reachability/ptpn_analysis.h`) drives exploration: `time_elapse`, `fire`, canonicalization modes (`equality`/`max-lower`/`intersection`), `ReductionMode` (NONE / UPPER_BOUND zone-inclusion merging — sound WCRT upper bound, `wcrt_lb` becomes unavailable), opt-in k-extrapolation, `--horizon` dwell-bound pruning, and symmetry reduction (quotient by verified net automorphisms; `exact=false` metrics). Extrapolation and symmetry are rejected for job-clock layouts (EDF/FIFO/LLF) — only sound for transition-indexed layouts of static/PIP/PCP/SRP rules. `FiringEdge::dwell_[min,max]` is the per-edge time weight all timing metrics build on.

### Scheduling policies (`reachability/scheduling_policy.h`)

A `FiringRule` hierarchy computes the admissible selected active set per state class. The state-class graph **branches on scheduling decisions**: each admissible selected active set (and each tied ordering for EDF/FIFO/LLF) yields one branch — not per interleaving. Rules:

- `StaticPriorityRule` — `fixed`, `fixed_prior_with_resume`, `fixed_prior_with_restart` (structural restart preemption), `rm`, `dm`, `pip`, `pcp`, `srp` (PIP/PCP/SRP require `task_place_capacity == 1`, one in-flight job; PCP/SRP use a global system ceiling)
- `EdfFiringRule` / `FifoFiringRule` — per-job release clocks; statically partitioned multi-core
- `LlfFiringRule` — event-driven LLF (laxity clocks), **not** classical continuous-time LLF; deterministic execution segments only

EDF/FIFO/LLF have an explicit **supported class**: static core partition, `cores_per_cpu == 1`, one in-flight job per task, no locks, no dependency-driven releases. Unsupported models are rejected explicitly (enforced in lowering and re-checked when the rule indexes the net) — see `docs/ptopner/scheduling-semantics.md` for the per-policy sections.

### Models and CLI

- `src/model/tdg.h` — TDG (tasks, fork/join, locks, dependencies, periodic release, `allowed_cores`, WCET provenance); format in `docs/json_format.md`
- `src/model/petri.h` — PTPN: `Transition` carries `TimeInterval`, `priority`, `core` (−1 = control), `suspendable`; `Marking` is `vector<int>`
- `src/dsl/` — RTSL lexer/parser/validator/lowering and `SourceMap`; spec in `docs/rtsl-spec.md`
- `src/app/main.cpp` — CLI11 subcommands `tdg`, `rtsl`, `check`, `ptpn`, `allocate`, `export romeo|ptopner`; options and defaults are documented in the README tables
- `src/app/pipeline.cpp` — analysis pipeline (`run_ptpn_postprocess`); also compiled into `ptpn_test` to expose `write_wcet_json`

TDG→PTPN lowering rules: `docs/rule.md`. Metrics schema/definitions: `docs/ptopner/metrics.md`; symbol index: `docs/ptopner/symbol-index.md`.

## Conventions and gotchas

- **`run.py` regex-parses `[STATS]` log lines** emitted by the analyzer (SCG build stats, lowering ms, etc.). Changing the text of those log lines breaks `run.py` silently — update `RE_*` patterns in `run.py` together with any format change.
- **Diagnostics are opt-in:** `--diagnostics json` / `--diagnostics-file PATH` switch `tdg`/`rtsl`/`check`/`ptpn`/`allocate` to the versioned envelope with exit codes `2/3/4/5/6` and move logs (including `[STATS]`) to stderr. Without them the legacy streams and `0`/`1` exit codes apply. `tools/llm measure` and the RTSL repair loop depend on the engaged form.
- **RTSL time is milliseconds:** RTSL inputs are normalized so one engine tick = 1 ms (bare integers follow `time_unit`; `us` is rejected). TDG JSON integers remain unit-agnostic engine ticks.
- Metrics JSON carries an `exact` flag: `false` under symmetry reduction, extrapolation, or horizon truncation, and `wcrt_lb` is omitted where the analysis is only an upper bound. Preserve that contract when touching metrics code.
- Tests map one `test_<concern>.cpp` per concern; policy tests exist for both single- and multi-core (`test_edf.cpp` + `test_edf_multicore.cpp`, etc.). Reuse `tdg_test_helpers.h` (`load_tdg`) rather than hand-parsing JSON in tests.
- Commit style is conventional commits (`feat(scheduling): ...`, `docs: ...`); the `style:` prefix is used for clang-format sweeps.
- `.gitignore` excludes `*.json`/`*.dot`/`*.ppn`/`*.ptpn` broadly; `test/golden/**` is explicitly re-included — keep golden fixtures committed.
- Namespaces are lowercase (`state_class`, `petri`, `app`, `converter`, `ptopner_export`, `romeo`); analysis code lives in `namespace state_class`.
- `tools/PToPNer` and `tools/romeo-2t` are git submodules (reference material only).
