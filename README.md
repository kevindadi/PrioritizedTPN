# PTPN

A **Priority Timed Petri Net** analyzer for real-time scheduling. It lowers task dependency graphs (TDGs) into timed Petri nets with priorities, preemption, and locks, then explores **state-class reachability graphs** (DBM clock zones) for analysis and export.

## Features

- Parse and validate **TDG JSON** (tasks, fork/join, locks, dependencies, periodic release)
- Lower TDGs into **PTPNs** (multi-core, fixed-priority scheduling, preemption, mutex/spin locks)
- Parse the **PTPN domain language** (`.ptpn`) directly, skipping TDG conversion
- **State-class reachability analysis** with canonicalization (`equality`, `max-lower`, `intersection`)
- **Sound upper-bound zone merging** (`--reduction upper-bound`): one-sided zone-inclusion merging for smaller graphs; WCRT stays a certified upper bound, `wcrt_lb` is reported as unavailable
- Optional **per-task cone decomposition** (`--reduction cone`): analyze each task on its dependency/contention cone; per-task timing and local-deadlock metrics match the full graph, but whole-net deadlock detection is unavailable and connected DAGs may not shrink
- Optional **WCRT time-horizon reduction** (`--horizon N`) that bounds SCG expansion and reports truncation/approximation explicitly
- Conservative **symmetry reduction** (`--reduction symmetry`) for verified isomorphic tasks on distinct cores
- **Core-allocation search** (`ptpn allocate`) over per-task `allowed_cores`, evaluating candidates with the exact state-class analysis and reporting feasible/Pareto assignments with an optimality status
- **LLM front-end prototype** (`tools/llm/`): natural-language task spec → validated task model → measured wall-clock interval → TDG JSON; measured values are explicitly not certified WCET
- Task `deadline` and WCET provenance (`wcet_kind`, `wcet_method`, `wcet_artifact`) in TDG JSON
- **Opt-in exports**: TDG / PTPN / state-class Graphviz DOT, WCET JSON, metrics JSON, Romeo CTS, PToPNer `.ppn`
- Dedicated **`export`** subcommands for Romeo and PToPNer workflows

## Input Modes

| Mode | Command | Input | Pipeline |
|------|---------|-------|----------|
| **TDG** | `ptpn tdg -f model.json` | Task dependency graph JSON | JSON → TDG → PTPN → analysis |
| **Direct** | `ptpn ptpn -f model.ptpn` | PTPN source file | `.ptpn` → PTPN → analysis |

```text
# TDG path (three parallel export/analysis pipelines)
JSON ──► TDG ──► tdg2pn ──► PTPN ──► State-Class Analysis / DOT / metrics
           │
           ├──► tdg2romeo ──► Romeo .cts
           └──► tdg2ptopner ──► PToPNer .ppn

# Direct path
.ptpn ──► Parser ──► PTPN ──► State-Class Analysis / DOT / metrics
```

## Quick Start

### Option A: Docker (Linux, vcpkg preconfigured)

The [Dockerfile](Dockerfile) builds on Ubuntu 24.04, bootstraps vcpkg, installs Boost from [vcpkg.json](vcpkg.json), runs the test suite, and ships a minimal runtime image.

```bash
# Native platform (arm64-linux on Apple Silicon Docker, x64-linux on Intel)
docker build -t ptpn .

# Force x86_64 Linux
docker build --platform linux/amd64 \
  --build-arg VCPKG_TARGET_TRIPLET=x64-linux -t ptpn .

# Analyze a TDG example
docker run --rm -v "$PWD/example:/data" ptpn tdg \
  -f /data/l-bench/input.json -m 1000 --export-scg /data/scg.dot
```

The runtime image contains `ptpn` and `ptpn_test` (`/usr/local/bin/`). Mount your inputs under `/data` (default `WORKDIR`).

### Option B: Native Build

#### Dependencies

- C++17
- CMake 3.27+
- Ninja (recommended)
- Boost (`graph`, `filesystem`, `thread`, `date_time`) — via **vcpkg** (recommended) or system packages

**Linux (Debian/Ubuntu) — system Boost:**

```bash
sudo apt install libboost-all-dev ninja-build cmake g++
```

**macOS — Homebrew:**

```bash
brew install boost ninja cmake
```

**vcpkg (recommended, matches CI/Docker):**

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
```

On **Apple Silicon**, ensure the vcpkg triplet matches your host (avoid the `x64-osx` / `arm64` mismatch that breaks linking):

```bash
cmake -B build -G Ninja -DVCPKG_TARGET_TRIPLET=arm64-osx   # Apple Silicon
cmake -B build -G Ninja -DVCPKG_TARGET_TRIPLET=x64-osx     # Intel Mac
```

CMake auto-detects vcpkg when `VCPKG_ROOT` is set and picks a host triplet if none is specified (see [CMakeLists.txt](CMakeLists.txt)).

#### Build

**Recommended** — helper script (bootstraps vcpkg if needed, writes `.ptpn-build-dir` so `run.py` finds the binary):

```bash
./build.py
./build.py --test          # build + ctest
./build.py --build-dir build-asan   # non-default build tree
```

**Manual CMake:**

```bash
cmake -B build -G Ninja
cmake --build build
```

Executables:

- `build/ptpn` — main analyzer
- `build/test/ptpn_test` — unit tests

`run.py` resolves the `ptpn` binary automatically:

1. `--ptpn PATH` if you pass it explicitly
2. environment variable `PTPN_BUILD_DIR` (CMake build directory)
3. `.ptpn-build-dir` at the repo root (written by `./build.py`)
4. fallback: `build/ptpn`

```bash
# typical workflow — no --ptpn needed
./build.py
python3 run.py                      # every experiment (p/s/t-bench + l-bench)

# custom build directory
./build.py --build-dir build-asan
export PTPN_BUILD_DIR=build-asan   # or rely on the updated .ptpn-build-dir marker
python3 run.py
```

#### Run Tests

```bash
ctest --test-dir build --output-on-failure
# or
./build/test/ptpn_test
```

## Usage

```bash
# Analyze from TDG JSON (reachability only, no files written)
./build/ptpn tdg -f example/p-bench/initial.json

# Analyze from PTPN source
./build/ptpn ptpn -f model.ptpn

# Help
./build/ptpn --help
./build/ptpn tdg --help
./build/ptpn export romeo --help
```

### Subcommands

| Subcommand | Purpose |
|------------|---------|
| `ptpn tdg` | Load TDG JSON, lower to PTPN, analyze and/or export |
| `ptpn ptpn` | Load `.ptpn` source, analyze and/or export |
| `ptpn export romeo` | Export Romeo CTS from TDG JSON (`--format scheduling-net\|inhibitor-arc`) |
| `ptpn export ptopner` | Export PToPNer `.ppn` (`-f` input, `-o` output) |

### Common Options (`tdg` / `ptpn`)

| Option | Description |
|--------|-------------|
| `-f, --file` | Input file (required) |
| `-m, --max-states` | Max states in reachability graph (default: `10000`) |
| `--canonicalization` | `equality` (default), `max-lower`, or `intersection` |
| `--keep-suspension-clocks` | Keep `w_t` in the DBM (default: project it out; exact optimization) |
| `--reduction` | `none` (default), `upper-bound` (sound WCRT upper bound, `wcrt_lb` unavailable), `cone` (per-task cone analysis; no whole-net deadlock), or `symmetry` (verified isomorphic tasks on distinct cores; pooled timing metrics are conservative) |
| `--extrapolation` | Enable opt-in k-extrapolation (timing metrics become approximate) |
| `--horizon N` | WCRT time-horizon reduction: do not expand state classes whose accumulated lower-bound dwell exceeds `N` (default: off). Sound for WCRT only when `N` bounds the response time of interest; deadlock/SCC results are then uncertified. See [docs/ptopner/metrics.md](docs/ptopner/metrics.md#time-horizon-reduction) |
| `--no-analysis` | Skip state-class reachability analysis |
| `--debug` | Enable debug logging |
| `-v, --version` | Show version |

**TDG-only:**

| Option | Description |
|--------|-------------|
| `--policy` | Override scheduling policy for lowering (see below) |

**Exports (all opt-in — paths are exactly what you pass):**

| Option | Output |
|--------|--------|
| `--export-tdg PATH` | TDG Graphviz DOT (TDG mode only) |
| `--export-wcet PATH` | Per-task WCET summary JSON (TDG mode only) |
| `--export-ptpn PATH` | PTPN structure Graphviz DOT |
| `--export-scg PATH` | State-class reachability graph DOT |
| `--export-metrics PATH` | Performance / schedulability metrics JSON |
| `--romeo PATH` | Romeo CTS (via `tdg2romeo`, TDG mode only) |
| `--ppn PATH` | PToPNer `.ppn` |
| `--tina PATH` | Tina `.net` (not implemented) |

> **Note:** Exports are **opt-in**. Running `ptpn tdg -f input.json` alone performs analysis but writes **no** output files unless you pass one or more export flags.

### Reduction Defaults and Measured Effect

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

### Core-Allocation Search (`ptpn allocate`)

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

The report contains:

- `search`: strategy (`exhaustive`/`heuristic`), whether the assignment domain
  was exhausted, domain size, evaluated/feasible counts, truncation, and reason;
- `best`: the highest-ranked feasible candidate (or `null`);
- `assignment`: the winning `{task, core}` list;
- `pareto`: non-dominated feasible candidates on WCRT, utilization, and load;
- `timing_model`: per-task WCET provenance and `all_certified`.

Feasibility requires an untruncated, bounded, deadlock-free, schedulable
analysis. `--require-certified-wcet` restricts `best` to tasks whose
`wcet_kind` is `certified`; measured/estimated/model timing is still analyzed
and reported but cannot win under that gate. See
[docs/ptopner/allocation.md](docs/ptopner/allocation.md) for the full schema and
ranking rules.

### LLM Front-End Prototype (`tools/llm`)

The prototype turns a validated task specification into LLM-proposed C
programs, compiles and runs them in a temporary directory, and emits a TDG with
`wcet_kind: "measured"`:

```bash
python -m tools.llm generate --spec spec.json --out model.json \
    --execute --runs 20 --timeout 5 --tdg-out graph.json
```

`--response-file` replays a saved model response without network access.
`--dry-run` prints the deterministic prompt.

**Trust boundary:** the LLM is an untrusted proposer. Its output is schema
validated; generated programs run only with `--execute`, with a timeout and
best-effort resource limits. The measured min/max wall-clock interval includes
process startup, has no confidence bound, and is **not a WCET upper bound**.
Do not use `wcet_kind=measured` for hard real-time claims; certified timing
requires a sound static WCET tool or validated MBPTA. Locked tasks require
`2 * lock_count + 1` per-segment measurements; the emitter rejects a single
total interval for them. See
[docs/llm-integration.md](docs/llm-integration.md).

### Symmetry Reduction (`--reduction symmetry`)

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

### Scheduling Policies

JSON `configuration.policy` and `--policy` accept values such as:

| Policy | Typical use |
|--------|-------------|
| `fixed` | Default fixed-priority lowering (resume-style native analysis) |
| `fixed_prior_with_resume` | Engine-native preemption (recommended for analysis) |
| `fixed_prior_with_restart` | Structural restart preemption (required for PToPNer export) |
| `fifo`, `rm`, `dm`, `edf`, `llf`, `pip`, `pcp`, `srp` | Other policies (see [docs/json_format.md](docs/json_format.md)) |

- **Native P-TPN analysis:** prefer `fixed` or `fixed_prior_with_resume` (tdg2pn lowering)
- **Romeo `.cts` export:** uses `tdg2romeo` directly from TDG; scheduling policy in JSON does not affect Romeo encoding
- **PToPNer `.ppn` export:** requires `fixed_prior_with_restart` (tdg2ptopner)

### Examples

```bash
# TDG: analyze + export state-class graph and PTPN DOT
./build/ptpn tdg -f example/l-bench/input.json \
  --export-scg scg.dot --export-ptpn ptpn.dot -m 5000

# TDG: larger benchmark (100 tasks, 20 cores, fork/join)
./build/ptpn tdg -f example/l-bench/input-complex.json -m 10000

# TDG: WCET summary only (no reachability)
./build/ptpn tdg -f example/p-bench/initial.json \
  --no-analysis --export-wcet wcet.json

# TDG: Romeo export (scheduling-net, default)
./build/ptpn export romeo -f example/p-bench/initial.json -o out.cts

# TDG: Romeo export (inhibitor-arc style)
./build/ptpn export romeo -f example/p-bench/initial.json -o out.cts --format inhibitor-arc

# TDG pipeline with inline Romeo export
./build/ptpn tdg -f example/p-bench/initial.json --no-analysis --romeo out.cts

# PTPN: max-lower canonicalization + metrics
./build/ptpn ptpn -f model.ptpn \
  --canonicalization max-lower --export-metrics metrics.json

# Standalone export subcommands
./build/ptpn export romeo -f example/p-bench/initial.json -o out.cts
./build/ptpn export ptopner -f example/p-bench/initial.json -o out.ppn \
  --policy fixed_prior_with_restart

# l-bench: scaling experiments (auto-finds build/ptpn via .ptpn-build-dir)
python3 run.py --lbench-cases pipeline-40t-8c-5l -m 5000000 --extrapolation
python3 run.py --reviewer-case -m 20000
python3 run.py --skip-suites --validate-tdg-only

# p/s/t-bench: export PTPN/SCG DOT, Romeo CTS, PToPNer .ppn
python3 run.py
python3 run.py --suites p-bench t-bench --profiles ptpn
```

## Example Inputs

| Directory | Contents |
|-----------|----------|
| [example/introduction/](example/introduction/) | Small introductory TDG |
| [example/motivating-examples/](example/motivating-examples/) | Motivating TDG examples |
| [example/p-bench/](example/p-bench/) | Priority / preemption benchmarks |
| [example/s-bench/](example/s-bench/) | Small scheduling benchmarks |
| [example/t-bench/](example/t-bench/) | Scalability benchmarks (`d-*.json`) |
| [example/l-bench/](example/l-bench/) | Progressive pipeline benchmarks (`pipeline-*t-*c.json`, 10–100 tasks, fork/join, scaled shared mutexes; `pipeline-40t-8c-5l.json` for the 5-lock reviewer case); legacy `input.json` / `input-complex.json` |

TDG JSON format: [docs/json_format.md](docs/json_format.md). PTPN language: [docs/ptpn-language-spec.md](docs/ptpn-language-spec.md).

## Documentation

| Document | Contents |
|----------|----------|
| [docs/json_format.md](docs/json_format.md) | TDG JSON input format |
| [docs/ptpn-language-spec.md](docs/ptpn-language-spec.md) | PTPN domain language |
| [docs/rule.md](docs/rule.md) | TDG → PTPN lowering rules |
| [docs/ptopner/](docs/ptopner/) | PToPNer export, scheduling semantics, metrics |
| [docs/tdg2romeo/](docs/tdg2romeo/) | TDG → Romeo conversion rules |
| [docs/romeo/](docs/romeo/) | Romeo engine semantics notes |

## Third-Party Dependencies

**Via vcpkg manifest** ([vcpkg.json](vcpkg.json)):

- Boost (`graph`, `filesystem`, `thread`, `date_time`)

**Fetched at configure time** (FetchContent):

- [CLI11](https://github.com/CLIUtils/CLI11) — CLI parsing
- [nlohmann/json](https://github.com/nlohmann/json) — JSON
- [spdlog](https://github.com/gabime/spdlog) / [fmt](https://github.com/fmtlib/fmt) — logging
- [GoogleTest](https://github.com/google/googletest) — tests

The [tools/PToPNer](tools/PToPNer/) tree holds PToPNer-related reference material.
