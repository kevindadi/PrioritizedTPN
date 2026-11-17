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

This repository is the **CLI-only engine**. The desktop workbench and all
user-facing documentation live in
[RT-PN](https://github.com/kevindadi/RT-PN):

- docs: <https://github.com/kevindadi/RT-PN/tree/main/docs/ptpn> (install,
  usage, RTSL reference, diagnostics catalog, architecture, TDG JSON format)
- workbench: <https://github.com/kevindadi/RT-PN>

## Build

Requirements: C++17, CMake >= 3.27, Ninja, Boost (`graph`, `filesystem`,
`thread`, `date_time`). nlohmann/json, CLI11, spdlog/fmt and GoogleTest are
fetched at configure time.

```bash
./example/build.py                 # bootstrap vcpkg if missing, configure + build
./example/build.py --test          # build + ctest
ctest --test-dir build --output-on-failure
```

CI-style Linux build with system Boost:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Windows: open the repository in CLion (the `CMakePresets.json` presets use
MinGW + vcpkg), or pass the vcpkg toolchain explicitly, for example
`-DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
-DVCPKG_TARGET_TRIPLET=x64-mingw-static`.

Docker (build context = repository root): `docker build -f Dockerfile -t ptpn .`.

## Usage

```bash
./build/ptpn check -f model.rtsl --diagnostics json    # parse + validate only
./build/ptpn rtsl  -f model.rtsl --export-tdg tdg.json
./build/ptpn tdg   -f tdg.json --export-ptpn net.dot --export-wcet wcet.json
```

`ptpn explain CODE` serves the diagnostic catalog; `ptpn describe -f ...`
restates the effective scheduling semantics.

## Layout

- `src/` — parsers, model, lowering, state-class analysis, metrics, exports, CLI
- `test/` — GoogleTest suites (run from the repository root)
- `test/reference/rtsl.ebnf` — grammar fixture kept in sync with the docs
- `example/` — models plus the `build.py` / `run.py` helpers
- `tools/gen_diagnostic_codes.py` — regenerates the diagnostic catalog include
  from `RT-PN docs/ptpn/diagnostics.md` (`--doc PATH` or
  `PTPN_DIAGNOSTICS_MD`)

## License

See [LICENSE](LICENSE).
