# TDG → Romeo Conversion Rules

This document describes how TDG JSON is lowered to Romeo `.cts` files through the standalone [`src/lower/tdg2romeo/`](../../src/lower/tdg2romeo/) module. It is **not** routed through `petri::PTPN`.

## Pipeline architecture

Three TDG export/analysis paths are parallel:

```
TDG JSON
  ├── tdg2pn/      → petri::PTPN   (P-TPN analysis model)
  ├── tdg2romeo/   → Romeo .cts
  └── tdg2ptopner/ → PToPNer .ppn
```

CLI entry points:

```bash
./build/ptpn export romeo -f example/p-bench/initial.json -o out.cts
./build/ptpn export romeo -f input.json -o out.cts --format inhibitor-arc
./build/ptpn tdg -f input.json --romeo out.cts --format scheduling-net
```

Options:

| Flag | Default | Meaning |
|------|---------|---------|
| `--format scheduling-net` | yes | Implicit core scheduling net (Romeo native style) |
| `--format inhibitor-arc` | | SwTPN-style immediate wrappers + `allow` guards |
| `--romeo-no-core-places` | off | Omit explicit `coreN` resource places |

## Shared TDG mapping

| TDG element | Romeo artifact |
|-------------|----------------|
| Task `T` with `time: [[a,b], ...]` | `Tentry`, `Tready`, `Texit` (+ segment/hold places if locks) |
| `configuration.start` | initial tokens on task entry places |
| `configuration.end` + sink tasks | `T_consume [0,0]` transitions |
| `configuration.periodic` | `T_period` place + `T_fire [P,P]` |
| Edge `S → T` with label `a,b` | `S_to_T [a,b]` bridge transition, `priority=0` |
| Lock `L` in task | lock place `L` (capacity 1), acquire/release on segment boundaries |
| Core assignment `core: C` | binds task to `coreC` place (when explicit core places enabled) |

Edge labels follow the same parsing rules as TDG2PN: `""` or `"0,0"` → immediate, `"a"` → `[a,a]`, `"a,b"` → `[a,b]`.

## Format 1: Scheduling net (`scheduling-net`)

Matches the implicit core scheduling net described in the paper (Fig. combined-c). Romeo stores priority on scheduling transitions and uses `VZone::firable` for same-core arbitration.

Per task `T(priority=P, core=C)`:

1. **`Tget_core [0,0]`** — `priority=P`
   - `when`: `Tentry >= 1` and (if core places) `coreC >= 1`
   - `intermediate`: decrement `Tentry` (+ decrement `coreC`)
   - update: `Tready += 1`

2. **`Texec [a,b]`** — `priority=P` (one per time segment)
   - `when`: current segment place `>= 1`
   - last segment also returns `coreC += 1` and releases held locks

3. **Bridge / periodic / consume** — `priority=0`

Multi-core isolation: each core gets `core0`, `core1`, … with initial marking 1. Tasks only guard/consume their assigned core place, so cross-core tasks do not compete through global priority alone.

## Format 2: Inhibitor arc (`inhibitor-arc`)

Approximates SwTPN / inhibitor-arc style (Fig. combined-b) in Romeo CTS syntax:

Per task `T`:

1. **`Tsched [0,0]`** — `priority=P`, optional `allow=…`
   - moves token `Tentry → Tready`, sets `Tactive = 1`
   - `allow` conjuncts:
     - `coreC_busy == 0` (core not occupied)
     - `H_active == 0` for every higher-priority task `H` on the same core

2. **`Texec [a,b]`** — **no** `priority` attribute
   - first segment sets `coreC_busy = 1`
   - last segment clears `coreC_busy` and `Tactive`

Timed execution intervals come from TDG `time` segments; priority is carried only on immediate scheduling wrappers.

## Differences from P-TPN (tdg2pn)

| Aspect | tdg2romeo | tdg2pn / P-TPN analysis |
|--------|-----------|-------------------------|
| Output | Romeo `.cts` | `petri::PTPN` |
| Preemption | priority + core places (Romeo engine) | resume / restart engine semantics |
| Suspend/resume | not modeled | native in analysis |
| WCRT / metrics | not computed by Romeo | computed by P-TPN analyzer |

Romeo is intended for qualitative state-class exploration and benchmark comparison, not for P-TPN quantitative metrics.

## Known limitations

- No mid-segment clock freeze / resume (Romeo engine limitation).
- Inhibitor format generates O(N²) same-core `allow` conjuncts for N tasks per core.
- Fork/join nodes are supported as zero-time control transitions; complex fork/join topology may need manual review.
- `cores_per_cpu > 1` is modeled as `coreN` initial marking = `cores_per_cpu`; per-core task binding remains one task chain per assigned core index.

## Regenerating benchmark artifacts

```bash
./build/ptpn export romeo -f example/s-bench/a.json -o example/romeo/s-bench/a.cts
python3 run.py --profiles romeo
```

Golden files live under [`example/romeo/`](../../example/romeo/).
