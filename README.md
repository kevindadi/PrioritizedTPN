# PTPN

A **Priority Timed Petri Net** analyzer for real-time scheduling. It lowers task dependency graphs (TDGs) into timed Petri nets with priorities, preemption, and locks, then explores **state-class reachability graphs** (DBM clock zones) for analysis and export.

## Features

- Parse and validate **TDG JSON** (tasks, fork/join, locks, dependencies, periodic release)
- Lower TDGs into **PTPNs** (multi-core, fixed-priority scheduling, preemption, mutex/spin locks)
- Parse the **PTPN domain language** (`.ptpn`) directly, skipping TDG conversion
- **State-class reachability analysis** with canonicalization (`equality`, `max-lower`, `intersection`)
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

**Recommended** — helper script (bootstraps vcpkg if needed, writes `.ptpn-build-dir` so other scripts find the binary):

```bash
./scripts/build.py
./scripts/build.py --test          # build + ctest
./scripts/build.py --build-dir build-asan   # non-default build tree
```

**Manual CMake:**

```bash
cmake -B build -G Ninja
cmake --build build
```

Executables:

- `build/ptpn` — main analyzer
- `build/test/ptpn_test` — unit tests

Other scripts (`run.py`, `run_lbench.py`, …) resolve the `ptpn` binary automatically:

1. `--ptpn PATH` if you pass it explicitly
2. environment variable `PTPN_BUILD_DIR` (CMake build directory)
3. `.ptpn-build-dir` at the repo root (written by `./scripts/build.py`)
4. fallback: `build/ptpn`

```bash
# typical workflow — no --ptpn needed
./scripts/build.py
python3 scripts/run.py
python3 scripts/run_lbench.py --all -m 5000000 --extrapolation

# custom build directory
./scripts/build.py --build-dir build-asan
export PTPN_BUILD_DIR=build-asan   # or rely on the updated .ptpn-build-dir marker
python3 scripts/run.py
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

# l-bench: generate progressive pipeline TDG JSON (10–100 tasks)
python3 scripts/generate_lbench.py --all
python3 scripts/generate_lbench.py --validate --all   # structure + TDG DOT only

# l-bench: run scaling experiments (auto-finds build/ptpn via .ptpn-build-dir)
python3 scripts/run_lbench.py --all -m 5000000 --extrapolation
python3 scripts/run_lbench.py --reviewer-case -m 20000
python3 scripts/run_lbench.py --all --validate-tdg-only

# p/s/t-bench: export PTPN/SCG DOT, Romeo CTS, PToPNer .ppn
python3 scripts/run.py
python3 scripts/run.py --suites p-bench t-bench --profiles ptpn
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

## Project Layout

| Module | Directory | Description |
|--------|-----------|-------------|
| Model | [src/model/](src/model/) | Types, TDG and PTPN data models |
| Parsing | [src/parse/](src/parse/) | TDG JSON parsing/validation and `.ptpn` language parsing |
| Exporters | [src/export/](src/export/) | PTPN structure / Graphviz export models |
| Lowering | [src/lower/](src/lower/) | TDG → PTPN / Romeo `.cts` / PToPNer `.ppn` |
| Analysis | [src/analysis/](src/analysis/) | State classes, DBM, scheduling, metrics |
| App | [src/app/](src/app/) | CLI entry and pipeline orchestration |
| Examples | [example/](example/) | TDG benchmark inputs |
| Tests | [test/](test/) | GoogleTest suite (`ptpn_test`) |

## Documentation

| Document | Contents |
|----------|----------|
| [docs/json_format.md](docs/json_format.md) | TDG JSON input format |
| [docs/ptpn-language-spec.md](docs/ptpn-language-spec.md) | PTPN domain language |
| [docs/rule.md](docs/rule.md) | TDG → PTPN lowering rules |
| [docs/ptopner/](docs/ptopner/) | PToPNer export, scheduling semantics, metrics |
| [docs/tdg2romeo/](docs/tdg2romeo/) | TDG → Romeo conversion rules |
| [docs/romeo/](docs/romeo/) | Romeo engine semantics notes |
| [CLAUDE.md](CLAUDE.md) | Developer / agent orientation |

## Third-Party Dependencies

**Via vcpkg manifest** ([vcpkg.json](vcpkg.json)):

- Boost (`graph`, `filesystem`, `thread`, `date_time`)

**Fetched at configure time** (FetchContent):

- [CLI11](https://github.com/CLIUtils/CLI11) — CLI parsing
- [nlohmann/json](https://github.com/nlohmann/json) — JSON
- [spdlog](https://github.com/gabime/spdlog) / [fmt](https://github.com/fmtlib/fmt) — logging
- [GoogleTest](https://github.com/google/googletest) — tests

The [tools/PToPNer](tools/PToPNer/) tree holds PToPNer-related reference material.
