# LLM front-end prototype

`tools/llm/` is a provider-agnostic prototype that turns a natural-language
task specification into:

1. a validated task model proposed by an LLM,
2. one measured wall-clock interval per task, and
3. a TDG JSON document that the existing PTPN parser and analysis pipeline can
   read.

The prototype uses Python 3.8+ and the standard library only. It does not add
dependencies to the C++ build.

## Trust boundary

**The LLM is an untrusted proposer.** Everything it returns is data:

- The model response is validated against a strict schema. Unknown or missing
  fields are errors, not warnings.
- Generated C sources are written to a fresh temporary directory, compiled
  with an argument list (never a shell), and executed only when the caller
  explicitly enables execution (`measure_task(..., allow_execute=True)` or
  `python -m tools.llm generate ... --execute`).
- Execution receives a wall-clock timeout and, on POSIX, best-effort
  `resource` limits (`RLIMIT_CPU`, `RLIMIT_AS`, `RLIMIT_FSIZE`). The child runs
  in its own session so a timed-out process group can be killed.
- API-key-shaped environment variables are removed from the environment passed
  to the compiler and to generated programs.
- The temporary directory provides working-directory separation, not a
  security sandbox. A generated program can still read other files that the
  invoking user can read and can use the network. Run the prototype inside a
  container or VM when the model is not trusted operationally.

**Measured execution time is not a WCET upper bound.**

- `measure_task` reports the minimum and maximum of `runs` wall-clock
  observations made with `time.perf_counter_ns()`. Values are integer
  microseconds.
- The interval includes process creation and Python `subprocess` overhead. It
  has no warm-up phase, no confidence bound, and no guarantee that the
  generated program exercised the true worst-case input.
- The emitted node fields say exactly this:
  - `wcet_kind: "measured"`
  - `wcet_method: "wall-clock repeated runs (prototype; not certified)"`
- `wcet_kind=measured` **must not** be used to claim hard real-time
  schedulability or any certified WCET guarantee. A certified upper bound
  requires a sound static WCET analysis tool or a measurement-based
  probabilistic timing analysis (MBPTA) process that has been validated for
  the target hardware. That validation is outside this prototype.

## Architecture and data flow

```text
spec.json
   |
   v
schema.validate_spec          strict spec validation
   |
   v
pipeline.build_prompt         deterministic prompt (sorted JSON, fixed text)
   |
   v
client.AnthropicClient / OpenAIClient / FakeClient / callable
   |
   v
client.extract_json_object    first JSON object, fences tolerated
   |
   v
schema.validate_model_response
   |
   +-- no --execute --> model.json + "needs_measurement" report (no TDG)
   |
   +-- --execute ----> pipeline.measure_task (cc -O2, temp dir, repeated runs)
                           |
                           v
                      pipeline.emit_tdg
                           |
                           v
                      TDG JSON --> existing PTPN parser/analysis
```

Modules:

| File | Responsibility |
| ---- | -------------- |
| `tools/llm/client.py` | Provider adapters, environment configuration, fake/injectable clients, strict JSON extraction. |
| `tools/llm/schema.py` | Strict spec and model-response validation; dependency-cycle detection. |
| `tools/llm/pipeline.py` | Prompt construction, measurement, and TDG emission. |
| `tools/llm/__main__.py` | CLI. |
| `tools/llm/tests/` | unittest suite; no network access. |

The **tool-side allocation explorer is a separate component**. It is not
implemented here; this prototype only proposes, measures, and emits the TDG
that such an explorer would consume.

## Spec format

Top-level fields (unknown fields are rejected):

| Field | Required | Type | Notes |
| ----- | -------- | ---- | ----- |
| `name` | no | string | Graph name; defaults to `LLMGeneratedTaskGraph`. |
| `num_cpus` | yes | integer >= 1 | Number of real cores; valid core ids are `0 .. num_cpus-1`. |
| `cores_per_cpu` | yes | integer >= 1 | Per-core capacity parameter; it does not create additional core ids. |
| `tasks` | yes | non-empty array | See below. |
| `shared_locks` | no | array of strings | If present, every task lock must be declared here. |
| `policy` | no | string | One of `fixed`, `rm`, `dm`, `edf`, `llf`, `fifo`, `pip`, `pcp`, `srp`; defaults to `fixed`. |
| `start` | no | array | Task names or `{"task": name, "tokens": int >= 0}`. |
| `end` | no | array of strings | End task names. |
| `periodic` | no | array | `{"task": name, "period": int > 0}` entries. |

Task fields (unknown fields are rejected):

| Field | Required | Type | Notes |
| ----- | -------- | ---- | ----- |
| `name` | yes | non-empty string | Unique; node id in the TDG. |
| `description` | yes | non-empty string | Natural-language task description sent to the model. |
| `period` | yes | integer >= 0 | `0` means non-periodic for default release binding. |
| `deadline` | yes | integer >= 0 | Absolute deadline; emitted on the task node. `0` means implicit (= period) or no deadline for aperiodic tasks. |
| `priority` | yes | integer | Emitted unchanged. |
| `allowed_cores` | yes | non-empty distinct integer array | Each core must be in range. |
| `locks` | no | array of strings | Names must start with `mutex` or `spin`; duplicates rejected. |
| `depends_on` | no | array of task names | Must exist, must not self-reference, and must remain acyclic. |

Example:

```json
{
  "name": "Demo",
  "num_cpus": 2,
  "cores_per_cpu": 1,
  "policy": "fixed",
  "tasks": [
    {
      "name": "A",
      "description": "Acquire and process one sample on the worst-case input.",
      "period": 100,
      "deadline": 100,
      "priority": 10,
      "allowed_cores": [0]
    },
    {
      "name": "B",
      "description": "Consume the processed sample.",
      "period": 0,
      "deadline": 0,
      "priority": 9,
      "allowed_cores": [0, 1],
      "locks": ["mutex0"],
      "depends_on": ["A"]
    }
  ]
}
```

## Prompt and response contract

`pipeline.build_prompt(spec)` is deterministic: it embeds the normalized spec
as sorted, indented JSON in a fixed template. The prompt asks for exactly one
JSON object and warns that each C program must:

- define `main` and include its own headers;
- exercise a representative worst-case path on every run;
- be deterministic, terminate, and exit with status 0;
- use no files, network, command-line input, or external project files.

The expected response shape is:

```json
{
  "tasks": [
    {
      "name": "A",
      "source": "int main(void) { /* ... */ return 0; }",
      "depends_on": ["A"],
      "locks": ["mutex0"],
      "allowed_cores": [0]
    }
  ]
}
```

Validation rules:

- Top-level and per-task unknown fields are rejected.
- `name` and `source` are required. `source` must be non-empty, contain a
  `main` definition, contain no NUL bytes, and be at most 1,000,000
  characters.
- Task names must be unique and, when a spec is supplied, must match the spec
  task set exactly (no missing or extra tasks).
- `depends_on` may reference only tasks in the model, must not self-reference,
  and must be acyclic after merging with the spec.
- `locks` must use `mutex`/`spin` prefixes.
- `allowed_cores` must be non-empty, distinct, and a subset of the spec's
  affinity for that task.

### Declaration precedence

| Declaration | If present in the model | If absent from the model |
| ----------- | ----------------------- | ------------------------ |
| `depends_on` | Replaces the spec hint. | Spec value is used. |
| `locks` | Replaces the spec hint. | Spec value is used. |
| `allowed_cores` | Narrows the spec affinity (subset enforced). | Spec affinity is used. |

Priority, period, deadline, and the graph-level configuration always come from
the validated spec.

## TDG emission

`emit_tdg(spec, generated, measured)` produces the same top-level shape used by
`docs/json_format.md`:

```json
{
  "graph": {"name": "..."},
  "configuration": {
    "num_cpus": 1,
    "cores_per_cpu": 1,
    "shared_locks": [],
    "policy": "fixed",
    "start": [],
    "end": [],
    "periodic": []
  },
  "nodes": [],
  "edges": []
}
```

Task nodes contain `type`, `id`, `priority`, `deadline`, `core` (first allowed
core), `time`, `allowed_cores`, `locks`, `wcet_kind`, and `wcet_method`.
Dependency edges use `{"source": <dependency>, "target": <task>, "label": "0,0"}`.

### Default activation

When `start`, `end`, or `periodic` is omitted from the spec:

- `start`: root tasks (no dependencies) with `period == 0` get one token.
- `periodic`: every task with `period > 0` gets a release binding.
- `end`: sink tasks (no outgoing dependency edges) are listed.

An explicit empty list is respected. For example, `"periodic": []` disables
automatic periodic release bindings.

### Locked tasks and time segments

The existing parser requires `2 * lock_count + 1` time intervals per task:
pre-critical-section, critical-section, and post-critical-section segments for
one lock, extended in the same pattern for nested locks. The emitter therefore
requires **per-segment measurements** for tasks that declare locks. A single
total interval is rejected with a `SchemaError`; silently placing it in the
first segment would model every critical section as zero-time and remove lock
contention from the analysis.

The measurement format for a task with `n` locks is a list of `2n + 1`
intervals, for example `[[20, 25], [25, 30], [5, 5]]` for one lock. Lock-free
tasks use one total interval. Producing segment-level timings requires
instrumenting the generated program or measuring separate phases; the prototype
does not infer them from a total wall-clock interval. Supply them with
`--measured-json` (the `--execute` path measures only a total interval and is
intended for lock-free tasks).

## Environment variables

| Variable | Required | Purpose |
| -------- | -------- | ------- |
| `PTPN_LLM_PROVIDER` | yes for live calls | `anthropic` or `openai`. |
| `PTPN_LLM_MODEL` | yes for live calls | Provider model identifier. |
| `PTPN_LLM_BASE_URL` | no | Override the API host/prefix. For OpenAI-compatible servers the tool appends `/v1/chat/completions` unless the URL already contains it. |
| `ANTHROPIC_API_KEY` | for `anthropic` | Sent as `x-api-key`. |
| `OPENAI_API_KEY` | for `openai` | Sent as a Bearer token. |

Keys are read from the environment only. They are never written to output
files, printed, or included in client `repr()` output. Use `--response-file`
for fully offline runs that need no key at all.

Anthropic requests use `https://api.anthropic.com/v1/messages` with
`anthropic-version: 2023-06-01`. OpenAI-compatible requests use
`/v1/chat/completions`.

## CLI

```text
python -m tools.llm generate --spec spec.json --out model.json \
    [--execute] [--runs N] [--timeout S] [--dry-run] \
    [--response-file RESPONSE] [--tdg-out TDG] [--compiler CC] [--opt FLAG]
```

| Option | Meaning |
| ------ | ------- |
| `--spec` | Input task specification. |
| `--out` | Validated model output path. |
| `--execute` | Compile and run every task, then emit a TDG. |
| `--runs` | Timed executions per task (default `20`). |
| `--timeout` | Wall-clock seconds per execution (default `5`). |
| `--dry-run` | Print the deterministic prompt; no provider call and no files written. |
| `--response-file` | Replay a saved LLM response, tolerating Markdown fences. No network. |
| `--measured-json` | JSON object mapping task names to intervals or per-segment interval lists. Supplied tasks are not executed; this is the supported path for locked tasks. |
| `--tdg-out` | TDG path when `--execute` or a complete `--measured-json` is supplied; default is `<out>.tdg.json`. |
| `--compiler` | C compiler executable (default `cc`). |
| `--opt` | Optimisation flag (default `-O2`). |

### Why a timing source is required for a TDG

Without measurement, any `time` interval would be fabricated. The CLI
therefore never emits a TDG claiming measured timing unless `--execute` or a
complete `--measured-json` was supplied. In the default offline path it writes
the validated model and prints a `needs_measurement` report:

```json
{
  "status": "needs_measurement",
  "model_out": "model.json",
  "tdg_out": null,
  "tasks": ["A", "B"],
  "message": "Model written but no TDG emitted: execution timing was not measured. ..."
}
```

Passing `--tdg-out` without `--execute` is an error (exit code 2).

### Examples

```bash
# Inspect the exact prompt without calling a provider.
python -m tools.llm generate --spec spec.json --out model.json --dry-run

# Live generation; validates and saves the model, but emits no TDG.
export PTPN_LLM_PROVIDER=anthropic
export PTPN_LLM_MODEL=your-model-id
export ANTHROPIC_API_KEY=...        # do not commit this
python -m tools.llm generate --spec spec.json --out model.json

# Replay a saved response and measure it (no network).
python -m tools.llm generate --spec spec.json \
    --response-file response.txt --out model.json \
    --execute --runs 20 --timeout 5 --tdg-out graph.json

# OpenAI-compatible local server.
export PTPN_LLM_PROVIDER=openai
export PTPN_LLM_MODEL=local-model
export OPENAI_API_KEY=not-needed
export PTPN_LLM_BASE_URL=http://localhost:8000
python -m tools.llm generate --spec spec.json --out model.json
```

Exit codes: `0` success, `2` configuration/spec/model/IO error, `3`
measurement error. Measurement errors are printed as structured JSON with
`stage`, `returncode`, `timed_out`, `stdout`, and `stderr`, so compiler
diagnostics are preserved.

## Measurement details and limitations

- Compilation command shape: `cc -O2 -std=c11 -Wall -Wextra -o task_bin
  task.c -lm`, executed with an argument list and `shell=False`.
- The source, binary, and working directory live in a fresh
  `tempfile.mkdtemp()` directory that is removed afterwards.
- Each run is a fresh process. Elapsed time is measured with
  `time.perf_counter_ns()` around process start and completion.
- Reported units are integer microseconds. The interval is `[min, max]` over
  the requested runs.
- POSIX best-effort limits: CPU time, 512 MiB address space, 1 MiB file size,
  and no core dumps. Limits that the host refuses to set are skipped; the
  wall-clock timeout still applies.
- Sensitive-looking environment variables (`*API_KEY*`, `*TOKEN*`, `*SECRET*`,
  `*PASSWORD*`, `*CREDENTIAL*`, `PTPN_LLM_*`) are removed from the child
  environment.
- Process start-up dominates very short tasks. This inflates the measured
  interval and is one reason it is not a WCET bound.
- No coverage or input-space exploration is performed, so a "worst-case path"
  claimed by the model is not verified.
- On non-POSIX hosts the resource limits and session handling are unavailable;
  only the wall-clock timeout remains.

## Tests

The suite uses `unittest` and never contacts a provider:

```bash
python -I -m unittest discover -s tools/llm/tests -v
```

Tests cover strict schema acceptance/rejection, fenced/prose JSON extraction,
fake-client generation, TDG shape and parser conventions, measurement disabled
by default, structured compiler errors, wall-clock timeout, and an offline CLI
round trip (execution tests are skipped when `cc` is unavailable).
