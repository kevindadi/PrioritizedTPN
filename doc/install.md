# Installing and Building PTPN

This page covers dependencies, native builds, tests, formatting, and the Docker
image. For command-line usage see [usage.md](usage.md).

## System dependencies

- C++17 compiler
- CMake 3.27+
- Ninja (recommended)
- Boost (`graph`, `filesystem`, `thread`, `date_time`) — via system packages or
  **vcpkg** (native builds)

Boost is declared in [vcpkg.json](../vcpkg.json) for manifest-mode vcpkg.
The following dependencies are fetched at configure time with `FetchContent`:
[CLI11](https://github.com/CLIUtils/CLI11) (CLI parsing),
[nlohmann/json](https://github.com/nlohmann/json) (JSON),
[spdlog](https://github.com/gabime/spdlog) / [fmt](https://github.com/fmtlib/fmt)
(logging), and [GoogleTest](https://github.com/google/googletest) (tests).
Python 3.8+ is only needed for `tools/llm` and the helper scripts.

**Linux (Debian/Ubuntu) — system Boost:**

```bash
sudo apt install libboost-all-dev ninja-build cmake g++
```

**macOS — Homebrew:**

```bash
brew install boost ninja cmake
```

**vcpkg (optional, native builds):**

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
```

vcpkg auto-detection is off by default on Linux and macOS. Re-enable it with
`-DPTPN_USE_VCPKG=ON` when `VCPKG_ROOT` is set. On **Apple Silicon**, pass a
triplet that matches the host to avoid the `x64-osx` / `arm64` mismatch that
breaks linking:

```bash
-DVCPKG_TARGET_TRIPLET=arm64-osx   # Apple Silicon
-DVCPKG_TARGET_TRIPLET=x64-osx     # Intel Mac
-DVCPKG_TARGET_TRIPLET=x64-linux   # x86_64 Linux
```

## Native build

### CMake (CI-style, system Boost)

This is the path CI uses on Linux:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

With vcpkg, point CMake at the toolchain file (and, on Apple Silicon, at the
host triplet):

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx
cmake --build build
```

Executables:

- `build/ptpn` — analyzer CLI
- `build/test/ptpn_test` — unit tests

### Helper script (`example/build.py`)

The helper script lives in `example/` and resolves the repository root from its
own location. It bootstraps vcpkg under `~/vcpkg` when it is missing (unless
`--skip-vcpkg` is given), configures CMake with Ninja and a host-matching
triplet, builds, and writes a `.ptpn-build-dir` marker at the repository root.

```bash
./example/build.py                 # configure + build
./example/build.py --test          # build + ctest
./example/build.py --skip-vcpkg    # use system Boost (what CI does on Linux)
./example/build.py --build-dir build-asan --build-type Debug --fresh --clean
./example/build.py --triplet arm64-osx --vcpkg-root /opt/vcpkg
```

| Option | Meaning |
|--------|---------|
| `--vcpkg-root PATH` | vcpkg installation directory (default: `~/vcpkg`) |
| `--build-dir PATH` | CMake build directory (default: `build/`) |
| `--triplet NAME` | vcpkg target triplet (default: auto-detected from host CPU/OS) |
| `--build-type TYPE` | `Debug`, `Release` (default), `RelWithDebInfo`, `MinSizeRel` |
| `-j, --jobs N` | Parallel build jobs (default: CMake/Ninja default) |
| `--clean` | Remove the build directory before configuring |
| `--fresh` | Pass `--fresh` to CMake configure (ignore the cache) |
| `--skip-vcpkg` | Do not install/bootstrap vcpkg; use system Boost |
| `--configure-only` | Configure only; skip the build |
| `--test` | Run `ctest` after a successful build |

`example/run.py` resolves the `ptpn` binary in this order:

1. `--ptpn PATH` if passed explicitly
2. `$PTPN_BUILD_DIR` (CMake build directory)
3. the `.ptpn-build-dir` marker at the repository root (written by
   `example/build.py`)
4. fallback: `build/ptpn`

## Tests

```bash
ctest --test-dir build --output-on-failure
# or the single GoogleTest binary
./build/test/ptpn_test
./build/test/ptpn_test --gtest_filter='Symmetry*'
```

Run the binary from the repository root: tests are discovered with
`gtest_discover_tests` and a working directory of the repo root, and they
reference `example/...` and `test/golden/...` relative to it. Golden
comparisons are skipped on builds that do not use libstdc++, so regenerate
goldens only on libstdc++ builds.

Python tests for the LLM prototype (stdlib `unittest`, run from the repo root):

```bash
python -m unittest discover tools/llm/tests
```

## Formatting

CI pins **clang-format 21.1.8** (installed with pip). Use that exact version;
other versions produce different output. Check the tree with:

```bash
find src test -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
  | xargs -0 clang-format --dry-run --Werror
```

## Docker

The image ([Dockerfile](Dockerfile)) builds on Ubuntu 24.04, installs system
Boost (`libboost-all-dev`) with apt, configures with CMake + Ninja, runs the
test suite during the build, and ships a minimal runtime image containing
`ptpn` and `ptpn_test` in `/usr/local/bin/`. The runtime `WORKDIR` is `/data`.

Run the build command from the repository root: the build context stays the
repository root, so the Dockerfile's `COPY` paths are unchanged.

```bash
# Native platform (arm64 on Apple Silicon Docker, amd64 on Intel)
docker build -f doc/Dockerfile -t ptpn .

# Force x86_64 Linux
docker build -f doc/Dockerfile --platform linux/amd64 -t ptpn .
```

Build arguments: `UBUNTU_VERSION` (default `24.04`) and `CMAKE_BUILD_TYPE`
(default `Release`).

Mount your inputs under `/data`; for example, mount the bundled `example/`
directory and analyze an l-bench case:

```bash
docker run --rm -v "$PWD/example:/data" ptpn tdg \
  -f /data/l-bench/pipeline-40t-8c-5l.json -m 1000 --export-scg /data/scg.dot
```
