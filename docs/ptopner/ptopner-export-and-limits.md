# PToPNer Export Limits and Failures

## Core claim

PToPNer export is not a generic "any TDG can be emitted" path. This repository validates a TDG against a narrower compatibility profile first, and only accepts models that match PToPNer's supported subset: `fixed_prior_with_restart`, point intervals only, and no lock modeling.

## Main export chain

The CLI path in `src/app/main.cpp` is:

1. Parse TDG JSON.
2. Build `tdg::TDG`.
3. Run `ptopner_export::validate_for_ptopner(tdg)`.
4. If validation passes, lower / export toward `.ppn`.

Relevant anchors in `src/app/main.cpp`:

```cpp
const auto ppn_validation = ptopner_export::validate_for_ptopner(tdg);
const auto ppn_export = ptopner_export::export_ptpn_to_ppn_file(ptpn, opts.ppn_file);
```

The validator implementation is in `src/lower/tdg2ptopner/validate.cpp`.

## Hard rejection rules

### 1. Scheduling policy must be `fixed_prior_with_restart`

`src/lower/tdg2ptopner/validate.cpp:187`:

```cpp
if (tdg.policy != SchedulePolicy::FIXED_PRIOR_WITH_RESTART) {
  result.errors.push_back("不支持调度策略 ... PToPNer 路径要求 fixed_prior_with_restart");
  result.ok = false;
}
```

If the TDG uses another policy, export is rejected immediately.

### 2. Every task / fork / join time must be a point interval

`validate_point_intervals` (`src/lower/tdg2ptopner/validate.cpp:22`) walks task-like nodes and rejects any interval with `min != max`.

So PToPNer export accepts deterministic durations, not general `[earliest, latest]` intervals.

### 3. No lock modeling is allowed

`validate_no_locks` (`src/lower/tdg2ptopner/validate.cpp:48`) rejects both:

- non-empty global `tdg.lock_set`
- task-local `task.lock`

This means the PToPNer path currently does not support shared-lock semantics.

## Warning-only behavior

`validate_warnings` (`src/lower/tdg2ptopner/validate.cpp:172`) emits warnings for two cases.

### Dashed edges are ignored

Dashed edges do not hard-fail export. They produce a warning:

- `虚线边 ... 将被忽略(与 tdg2pn 一致)`

So the generated `.ppn` intentionally drops them.

### Periodic configuration is reused from `tdg2pn`

Periodic tasks also do not hard-fail export. They produce a warning that the exporter reuses the existing `tdg2pn` period-release modeling.

This is a compatibility notice, not a rejection reason.

## What an export failure means

If `validate_for_ptopner(...)` returns `ok = false`, the problem is not a generic parser error. It means the TDG is outside the subset that this repository knows how to translate into PToPNer-compatible `.ppn` output.

Typical failure explanations are therefore semantic / compatibility explanations, not syntax explanations:

- wrong scheduling policy
- non-point execution times
- lock usage

## Key files

- `src/app/main.cpp:230` — validation call in CLI path
- `src/lower/tdg2ptopner/validate.cpp:22` — point-interval validation
- `src/lower/tdg2ptopner/validate.cpp:48` — lock rejection
- `src/lower/tdg2ptopner/validate.cpp:78` — warning-only cases
- `src/lower/tdg2ptopner/validate.cpp:187` — top-level `validate_for_ptopner`

## FAQ-style quick answers

- "Why can't this TDG export?" → first check policy, intervals, and locks.
- "Do dashed edges block export?" → no; they are ignored with a warning.
- "Do periodic tasks block export?" → no; they warn and reuse existing release modeling.
- "Why are point intervals required?" → because this export path targets a narrower deterministic subset than the full PTPN analysis path.

## Further reading

- Analysis/scheduler semantics: `docs/ptopner/scheduling-semantics.md`
- Pipeline overview: `docs/ptopner/architecture.md`
- Full lowering rules: `docs/rule.md`
- External tool context: `tools/PToPNer/README.md`
