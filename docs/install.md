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
Python 3.8+ is only needed for the helper scripts (`example/build.py`,
`example/run.py`).

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

**Windows (MinGW-w64 + vcpkg):**

- MinGW-w64 (GCC 15) — CLion bundles one at `<CLion>/bin/mingw`; a standalone
  x86_64 MinGW-w64 toolchain works too
- CMake 3.27+ and Ninja — the copies bundled with CLion work
- vcpkg checked out at `D:\vcpkg`; the manifest dependencies are installed into
  the build tree (`cmake-build-debug-mingw/vcpkg_installed`), not globally

[`CMakePresets.json`](../CMakePresets.json) sets `VCPKG_ROOT`, the
`x64-mingw-dynamic` triplet, and CLion's default build directories
(`cmake-build-debug-mingw` / `cmake-build-release-mingw`). Put the toolchain's
`bin` directory on `PATH` (CLion's terminal does this when the MinGW toolchain is
selected):

```bat
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Change `environment.VCPKG_ROOT` in `CMakePresets.json` when vcpkg lives
elsewhere. `compile_commands.json` is linked/copied to the repository root for
clangd; when symlink creation is not permitted (no Developer Mode), CMake
copies the file instead.

**CLion:** open the repository. CLion loads `CMakePresets.json` under
*Settings ▸ Build, Execution, Deployment ▸ CMake*; select the **MinGW**
toolchain (the bundled one is preconfigured) and the `windows-debug` preset,
then reload the project. Builds go to CLion's default `cmake-build-debug-mingw`
directory. The `ptpn` and `ptpn_test` targets and the discovered GoogleTest
cases appear in the run and test configurations. vcpkg runs automatically
during the first configure (Boost is built for MinGW once).

The golden-output tests are skipped on Windows: the committed fixtures are
LF-only outputs produced by libstdc++ on POSIX, and text-mode writes add CRLF.

### Desktop analyzer (Qt 6, optional)

The optional Qt Widgets workbench lives in [`desktop/`](../desktop/README.md).
It runs the built `ptpn` CLI and presents the DAG, PTPN, state-class, timeline
and report stages as text-first views; the PTPN/SCG graphs are explicit
secondary previews. A first-run chooser separates specification-first intake
from binary-first intake, where an ELF is paired with a `<elf>.dag.json` sidecar
from an external static analyzer. It is off by default (`PTPN_BUILD_DESKTOP`) so
the analyzer and tests never require Qt.

Build the reduced Qt base port once (Widgets + PNG only), then configure the
desktop preset:

```bat
cd desktop
D:\vcpkg\vcpkg.exe install --triplet x64-mingw-dynamic ^
  --host-triplet x64-mingw-dynamic --x-install-root=D:\vcpkg\installed
cd ..
cmake --preset windows-desktop
cmake --build --preset windows-desktop --target ptpn_desktop
cmake-build-desktop-mingw\desktop\ptpn_desktop.exe
```

In CLion select the `windows-desktop` preset with the MinGW toolchain. The
post-build step copies the Qt/MinGW runtime next to `ptpn_desktop.exe`. See
[desktop/README.md](../desktop/README.md) for details and the `D:\mingw64`
junction workaround for `windres` and paths containing spaces.

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

The Windows presets place the same binaries under `cmake-build-debug-mingw/`
(or `cmake-build-release-mingw/`).

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
comparisons are skipped on builds that do not use libstdc++ and on Windows
(CRLF), so regenerate goldens only on POSIX libstdc++ builds.

## Formatting

There is no `.clang-format` config and no CI format gate (both were removed
deliberately). Match the surrounding code style when editing.

## Docker

The image ([Dockerfile](Dockerfile)) builds on Ubuntu 24.04, installs system
Boost (`libboost-all-dev`) with apt, configures with CMake + Ninja, runs the
test suite during the build, and ships a minimal runtime image containing
`ptpn` and `ptpn_test` in `/usr/local/bin/`. The runtime `WORKDIR` is `/data`.

Run the build command from the repository root: the build context stays the
repository root, so the Dockerfile's `COPY` paths are unchanged.

```bash
# Native platform (arm64 on Apple Silicon Docker, amd64 on Intel)
docker build -f docs/Dockerfile -t ptpn .

# Force x86_64 Linux
docker build -f docs/Dockerfile --platform linux/amd64 -t ptpn .
```

Build arguments: `UBUNTU_VERSION` (default `24.04`) and `CMAKE_BUILD_TYPE`
(default `Release`).

Mount your inputs under `/data`; for example, mount the bundled `example/`
directory and analyze an l-bench case:

```bash
docker run --rm -v "$PWD/example:/data" ptpn tdg \
  -f /data/l-bench/pipeline-40t-8c-5l.json -m 1000 --export-scg /data/scg.dot
```
