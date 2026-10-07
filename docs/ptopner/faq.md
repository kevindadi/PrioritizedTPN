# PTPN / PToPNer FAQ

## 状态类的具体算法是什么？

短答:它不是"先把所有时钟一起推进,再任选一个使能变迁".这里的状态类展开分成三步:

1. 从当前 `marking` 计算 `enabled`.
2. 通过调度规则得到 `active` / `suspended`.
3. 对可运行变迁计算最早可发生时间,只从最早那一批里再做按核心优先级筛选,然后单步 firing.

代码锚点:

- `src/analysis/graph.h:70` — `build`
- `src/analysis/graph.h:96` — `advance_time`
- `src/analysis/graph.h:107` — `fire_with_time`
- `src/analysis/graph.h:118` — `recompute_enabled_sets`

文档入口:`docs/ptopner/time-and-state-class.md` 与 `docs/ptpn-formal-semantics.md` §7–§8.

## `E` / `X` / `R` 在代码里分别对应什么？

- `E` → `StateClass.enabled` in `src/analysis/state.h:59`
- `X` → `StateClass.active` in `src/analysis/state.h:60`
- `R` → `StateClass.suspended` in `src/analysis/state.h:61`

这里和 Roméo 的关键差异是:本仓库把这些集合显式存进状态;Roméo 更偏向把优先级效果隐含在 timed firability 的约束里.

文档入口:`docs/ptopner/scheduling-semantics.md`.

## 时间是在哪里推进的？

`StateClassReachabilityGraph::advance_time` in `src/analysis/graph.h:96`.

它的规则是:找到 `active` 集合中最紧的上界,只推进 `active` 时钟;`suspended` 时钟保持冻结,并把推进量累计到 `cumulative_time`.

文档入口:`docs/ptopner/time-and-state-class.md`.

## 挂起和恢复的判定逻辑在哪？

在 `src/analysis/scheduling.cpp`:

- `select_active_per_core` — 每核保留最高优先级(可并列)
- `compute_suspended` — 计算哪些 enabled 但非 active 的变迁应被冻结
- `should_suspend` / `should_restore` — 单个变迁的挂起/恢复判断

核心条件是:同核心、更高优先级、且低优先级变迁本身 `suspendable == true`.

文档入口:`docs/ptopner/scheduling-semantics.md`.

## 为什么某个 TDG 不能导出到 PToPNer？

先看 `src/lower/tdg2ptopner/validate.cpp` 的 `validate_for_ptopner`.最常见的三类硬失败是:

1. 调度策略不是 `fixed_prior_with_restart`
2. 存在非点区间 `min != max`
3. 使用了锁

文档入口:`docs/ptopner/ptopner-export-and-limits.md`.

## 虚线边和 periodic 会直接导致导出失败吗？

不会.

- 虚线边:warning,并在导出时忽略
- periodic:warning,并复用 `tdg2pn` 的 period release 建模

这两类行为都在 `src/lower/tdg2ptopner/validate.cpp:172` 的 warning 逻辑里.

## 这里和 Roméo 的 priority / time 处理差异是什么？

短答:

- **Roméo**:优先级更多隐含在 timed symbolic firability 里,通过 zone / DBM 约束把低优先级 firing 排除掉.
- **本仓库**:把 `enabled` / `active` / `suspended` 做成显式状态分量,再结合 DBM 与冻结时钟表达时间推进.

所以回答"当前哪些变迁在跑、哪些被冻结、为什么"时,这个仓库通常可以直接从 `StateClass` 解释;Roméo 往往要从 timed-domain 约束侧解释.

对应文档:

- `docs/ptopner/scheduling-semantics.md`
- `docs/ptopner/time-and-state-class.md`
- `docs/romeo/priority-semantics.md`
- `docs/romeo/time-semantics.md`
