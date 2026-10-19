# RTSL 规格 v1（设计冻结稿）

> 状态：v1 已实现并合入 `ptpn`（Phase 2–3）；本稿冻结语法与语义，§14 记录 v2+ TODO。
> 关联文档：[docs/proposal-llm-dsl.md](proposal-llm-dsl.md)（方案与约束）、[docs/diagnostics.md](diagnostics.md)（诊断协议）、[docs/json_format.md](json_format.md)（TDG JSON 语义参考）。
>
> **实现状态**（阶段对应 [proposal](proposal-llm-dsl.md) §8）：
>
> | 规格范围 | 实现阶段 |
> |---|---|
> | §2–§4、§8–§9：词法/语法/验证、规范化命名、SourceMap | Phase 2 |
> | §5–§7、§10：单位、core 门控、测时三模式、DSL→TDG 映射 | Phase 2–3 |
> | §11：`check` / `rtsl` / `allocate` | Phase 2–3 |
> | §12–§13：诊断码与测试 | Phase 1–3 |
> | 测时闭环（`tools/llm measure`） | Phase 3 |
> | §14：TODO | v2+ |

RTSL（Real-Time Systems Language）是 PTPN 项目的面向人/LLM 的实时系统描述语言。它**只是前端**：进程内 lowering 到现有 `model::TDG`（不经 JSON 文本往返），分析语义完全由现有 state-class 引擎承担（约束 C1）；TDG JSON 仍是一等公民（约束 C4）。

## 1. 目标与原则

1. **声明式**：描述系统是什么（任务、依赖、核心、时序来源），不描述怎么跑。
2. **三种读者**：用户手写可读、LLM 生成容易、解析器报错精确到行列且**一次报全**（panic-mode）。
3. **确定性**：任何输出（隐式节点名、TDG 字段）只由输入拓扑决定，与文件编辑顺序无关。
4. **时序来源是语法**（provenance 一等公民）；`measured ≠ certified` 信任边界贯穿。
5. **与 TDG 1:1**：每个构造对应现有 TDG 字段，无信息损失、无第二套语义。

## 2. 词法

- **标识符** `[A-Za-z_][A-Za-z0-9_]*`。**`__` 保留给隐式节点**：用户命名（任务/锁）以 `__` 开头**或包含 `__`** → E-VAL-202（与 §8 的 `__` 分隔符配合，数学上保证规范化命名单射、无碰撞）。
- **整数**：十进制非负。
- **时间字面量**：`INT` 后可跟单位后缀 `ms` | `s`（裸整数按 `time_unit` 折算；带后缀绝对，见 §5）。词法层识别任意字母后缀：`us` → E-UNS-004（亚毫秒精度不支持，见 §5），其他未知后缀 → E-SYN-101。
- **字符串**：`"..."`（`\"`、`\\`、`\n` 转义），用于 provenance 附注与 artifact。
- **注释**：`//` 行注释、`/* ... */` 块注释（可嵌套不要求）。
- **关键字**（保留，不可作任务/锁名）：`system cpus cores_per_cpu time_unit policy task normal periodic aperiodic sporadic interrupt period deadline mit priority core any locks time measured estimated certified by llm runs timeout artifact start end`。**策略名不是关键字**：`policy` 后接任意 IDENT，语义层校验（§3/§12；`task fifo_buffer` 合法）。
- 标点：`{ } , : ; -> [ ]`。分号**可选**（语句边界由关键字与 `}` 判定，LLM 友好）。

## 3. 文法

```ebnf
program      := "system" IDENT "{" system_item* "}"
system_item  := system_attr | task_decl | binding | edge
system_attr  := "cpus" INT
              | "cores_per_cpu" INT
              | "time_unit" IDENT   // 单位校验与 TIME 后缀共用（§5）：ms/s 接受，us → E-UNS-004，未知 → E-SYN-101
              | "policy" IDENT
task_decl    := "task" IDENT (":" TASK_KIND)? "{" task_attr* "}"
TASK_KIND    := "normal" | "periodic" | "aperiodic" | "sporadic" | "interrupt"
task_attr    := "period" TIME | "deadline" TIME | "mit" TIME
              | "priority" INT
              | "core" core_spec
              | "locks" IDENT ("," IDENT)*
              | "time" time_decl
core_spec    := INT | "any" | "{" INT ("," INT)* "}"
time_decl    := intervals? provenance
intervals    := interval | "[" interval ("," interval)* "]"   // 多段（锁定任务：2*lock_count+1 段）
interval     := "[" TIME "," TIME "]"
TIME         := INT UNIT?
UNIT         := "ms" | "s" | "us"   // 词法只识别后缀；语义层分类（§5）：ms/s 接受，us → E-UNS-004，其他 → E-SYN-101
provenance   := "estimated" ("by" STRING)?
              | "certified" "by" STRING ("{" "artifact" STRING "}")?
              | "measured" "by" "llm" "{" measure_attr ("," measure_attr)* "}"
measure_attr := "runs" INT | "timeout" INT | "artifact" STRING
binding      := "start" IDENT | "end" IDENT
edge         := edge_src "->" edge_dst
edge_src     := IDENT | "{" IDENT ("," IDENT)* "}"
edge_dst     := IDENT | "{" IDENT ("," IDENT)* "}"
```

`time` 的区间在 `measured` 模式下可省略（未测时），其余模式必填（见 §7）。

### 3.1 Panic-mode 恢复（一次报全）

- `task { ... }` 内报错：跳到下一个属性关键字（`period deadline mit priority core locks time }`）继续解析；
- 顶层报错：跳到下一个系统/顶层关键字（`cpus cores_per_cpu time_unit policy task start end`）、**行首**（新行第一个 token 为 IDENT 或 `{`——保证文件末尾的边序列中一条边出错不会吞掉后续边）、或**同层级**的 `}` 继续；跳过时维护花括号深度（`core {a, b}` / 集合端点内的 `}` 不是同步点）；
- 词法层不致命失败：非法字符 → 错误 token + E-SYN 诊断，继续扫描；
- **级联抑制**：解析失败的构造标记 tainted（复用 `Diagnostics::mark_tainted`），校验阶段跳过依赖该构造的检查；无关校验照常报告；
- 单次上限 10 条 + `truncated_diagnostics`（沿用诊断协议）。

## 4. 语义与验证

### 4.1 system 块

| 属性 | 规则 | 默认 |
|---|---|---|
| `cpus N` | N ≥ 1，合法 core id 为 `0..N-1` | **必填** |
| `cores_per_cpu N` | N ≥ 1（容量参数，非额外 core 范围） | 1 |
| `time_unit` | `ms` \| `s`（`us` → E-UNS-004，见 §5） | `ms` |
| `policy` | 后接 IDENT，取值为 JSON 同款策略字符串（`fixed`、`fixed_prior_with_resume`、`rm`、`dm`、`pip`、`pcp`、`srp`、`edf`、`fifo`、`llf` 等）；语义层复用 `parse_schedule_policy`（未知 → E-VAL-215） | `fixed` |

### 4.2 任务类型（TASK_KIND）

| 种类 | 必带属性 | lowering | 说明 |
|---|---|---|---|
| `normal`（默认，`: normal` 可省） | — | `TaskType::NORMAL` | 引擎语义与 aperiodic 相同 |
| `periodic` | `period` | `TaskType::NORMAL` + `periodic_tasks` 绑定 | `deadline` 省略时 = period |
| `aperiodic` | — | `TaskType::NORMAL` | 保留两种写法便于表达，引擎不区分 |
| `interrupt` | — | `TaskType::INTERRUPT` | **v1 语义 = 非周期任务 + 类型标记**；`preemption.cpp` 的 `is_interrupt` 钩子目前是 stub（参数未使用）。`ptpn check` 对此发 hint 级诊断说明。真正的 interrupt 抢占语义属于分析核心扩展（约束 C1），排入 TODO |
| `sporadic` | `mit` | **未实现** | 语法保留（文件不因后续实现而改）；`check` 发 E-UNS-003，`fix` 建议 `periodic { period = mit }` 保守替代（最密到达模式，WCRT 合法上界） |

kind 互斥：一个 task 只能有一种 kind；`periodic` + `interrupt` 组合排入 TODO。

### 4.3 任务属性

| 属性 | 规则 |
|---|---|
| `period T` | 仅 periodic；T > 0（E-VAL-206） |
| `mit T` | 仅 sporadic；T > 0（E-VAL-207） |
| `deadline T` | T > 0（E-VAL-208）；periodic 且省略时 = period；非 periodic 的 deadline 与 JSON 语义一致（绝对截止期） |
| `priority N` | 默认 100（与 JSON 一致） |
| `core` | 见 §6 |
| `locks` | **隐式注册**到 lock 集合（无需 system 级声明）；前缀规则沿用 JSON（`mutex`/`spin`，E-VAL-014 复用） |
| `time` | 见 §7 |

### 4.4 依赖边与 fork/join

- `A -> B`：前驱边；`A -> {B, C}`：fork；`{A, B} -> C`：join。
- fork/join 为**隐式控制节点**（zero-time、control core -1），命名见 §8。**单元素集合在任何一侧都折叠**为普通边（`{A} -> B`、`A -> {B}`、`{A} -> {B}` 均等价于 `A -> B`）；**多对多边**（源与目标元素数均 ≥ 2，如 `{A, B} -> {C, D}`）→ E-VAL-216，提示拆分为显式中间任务。
- **重复边**（端点集合相同，顺序不同也算，如 `{fusion, log}` vs `{log, fusion}`）→ E-VAL-203，报出两处位置。
- 边引用未知任务 → E-VAL-204。
- fork/join 的自定义属性（time/core/priority，JSON 支持）→ v1 不支持，TODO。

## 5. 时间与单位

- **基准 ms**：所有模型时间在 lowering 时统一折算为 ms 整数（引擎时间不变，仍为整数 tick；RTSL 输入时 tick = ms，见 `docs/diagnostics.md` 无关、写入本文档与 README）。
- 字面量后缀 `ms`/`s` 绝对；裸整数按 `time_unit` 折算。
- **`us` 后缀** → E-UNS-004：低于基准分辨率，静默舍入对认证工具不可接受；亚毫秒建模需引擎 64 位时间（约束 C1），TODO。单位分类在**语义层**完成（词法只识别后缀），保证 `us` 报 E-UNS-004 而非语法错误。
- `timeout`（测时配置）是**墙钟秒**（INT），不是模型时间。
- TDG JSON 路径不受影响（其整数按现状解释为引擎 tick）。
- metrics 输出保持引擎 tick；RTSL 输入时 tick = ms，skill 在人类可读报告中做单位渲染。

## 6. 核心分配与子命令门控

| 语法 | lowering | 门控 |
|---|---|---|
| `core N` | `nodes[].core = N` | 全部子命令可用；N ∈ `0..cpus-1`（E-VAL-209） |
| `core {a, b, ...}` | `allowed_cores = {a,b,...}`，`core = min(集合)`（仅在不用 allocate 直接分析时生效；文档注明） | 空集合/重复/越界 → E-VAL-210/209；check、rtsl、allocate 可用 |
| `core any` | `allowed_cores = [0..cpus)` | `check` → hint 级诊断"需用 ptpn allocate"；`rtsl` → E-VAL-218（fix: use `ptpn allocate`）；`allocate` → 展开后搜索 |

## 7. 测时三模式

| 模式 | 语法 | `wcet_kind` | 规则 |
|---|---|---|---|
| LLM 测时 | `time [ [a,b], ... ]? measured by llm { runs N, timeout T, artifact? STR }` | `measured` | 区间可省略（未测状态）：`check` 接受；`rtsl`/`allocate` 对缺区间报 E-VAL-213（fix: run the measurement step）。测完后 skill **原位注入**区间与 artifact（二进制哈希 + 编译命令快照），保留 runs/timeout。多段测量与原型一致（锁定任务 2*lock_count+1 段） |
| 用户预估 | `time [ [a,b], ... ] estimated by STR?` | `estimated` | 区间必填（E-VAL-214） |
| 工具认证 | `time [ [a,b], ... ] certified by STR { artifact? STR }` | `certified` | 区间必填；`allocate --require-certified-wcet` 门控自动作用 |

`.rtsl` 是唯一事实源：测量只改 `.rtsl` 一处；TDG JSON 是派生产物。区间合法性（min ≤ max、段数 = 2*lock_count+1）沿用现有校验语义（E-VAL-212 / 复用 E-VAL-015 语义）。

`measured` 配置默认与约束：`runs` 默认 20、`timeout` 默认 5（墙钟秒），二者必须为正整数（非正 → E-VAL-220）；`artifact` 可选；measure 子属性重复声明 → E-VAL-219。

## 8. 隐式节点规范化命名（Canonical Naming）

- fork：`__fork_<src>__<t1>__<t2>…`，目标任务**按名字典序**排序；
- join：`__join_<s1>__<s2>…__<dst>`，源任务按名字典序排序；
- 名称只由拓扑端点决定：文件其他位置的增删不影响既有名称（golden 稳定性根基）；
- 碰撞安全（数学保证）：用户标识符禁止包含 `__`（E-VAL-202）⇒ 以 `__` 连接端点的编码是**单射**（反解 `__fork_<src>__<t1>__…` 时按 `__` 切分唯一）；重复边为验证错误（E-VAL-203）⇒ 规范化名天然唯一；
- 每个隐式节点在 SourceMap 中绑定到**对应边声明所在的行**（诊断指向用户写的边）。

## 9. SourceMap

`dsl::SourceMap`：旁挂字典（不污染 `model::TDG`），键 = 结构化 subject，值 = `SourceSpan{file,line,column,span}`。任何阶段（解析/校验/lowering 守卫）产出的 Diagnostic 只带 subject；发射器查表反填 `location`；查不到则省略（协议允许）。

键族：

```
task:<id>  task:<id>:core  task:<id>:time  task:<id>:period
task:<id>:deadline  task:<id>:mit  task:<id>:priority
lock:<name>
edge:<s1>,<s2>-><t1>,<t2>        // 源与目标均按字典序、逗号连接；单端点为单元素形（如 edge:sensor->fusion,log）
fork:<canonical-name>  join:<canonical-name>   // 绑到边声明行
policy  system:cpus  system:cores_per_cpu  system:time_unit
start:<t>  end:<t>
```

**`task:<id>:time` 的 span 约定**：覆盖**整条 time 属性**——从 `time` 关键字起，到该属性结束（measure 块的右花括号 `}` 或 provenance 结尾）。注入/更新采用**整段切片替换**：tools/llm 将 `time [ [a,b], ... ] measured by llm { runs N, timeout T, artifact "..." }` 格式化为完整字符串，对 span 做单次字节替换——首次注入区间、更新 artifact、重新测时覆盖旧区间，全部走同一操作。

## 10. DSL → TDG 映射总表

| RTSL | TDG 模型 / JSON 字段 |
|---|---|
| `cpus N` / `cores_per_cpu N` | `num_cpus` / `cores_per_cpu` |
| `time_unit U` | 仅解释裸整数；lowering 后全为 ms 整数 |
| `policy P` | `SchedulePolicy`（`configuration.policy`） |
| `task T: periodic { period }` | node `T` + `periodic_tasks` 绑定 |
| `task T: aperiodic/normal/interrupt` | node `T`（`TaskType::{NORMAL, INTERRUPT}`） |
| `period/deadline/priority` | `TaskNode::{period_binding, deadline, priority}` |
| `core N` / `core {a,b}` / `core any` | `core` / `allowed_cores` / 全域 |
| `locks L1, L2` | `task_locks_map` + `lock_set`（隐式注册） |
| `time … <来源>` | `time` 分段 + `WcetProvenance{kind,method,artifact}` |
| `A -> B` / fork / join | `tdg_edges` + 隐式 fork/join 节点（规范化命名） |
| `start T` / `end T` | `start_tasks`（tokens=1）/ `end_tasks` |

## 11. CLI

- **`ptpn check FILE.rtsl`**（新子命令）：词法 → 语法（panic-mode）→ 语义验证，**不做 lowering、不做分析**。这是有意拆分：`check` 只回答“模型是否可解析、语义是否自洽”，因此支持类别守卫（E-UNS-001）不在 check 阶段触发——它们属于 lowering，只在 `rtsl`/`allocate` 阶段出现；同理，未测时的 `measured` 在 check 中接受，E-VAL-213 由 `rtsl`/`allocate` 报出；`core any` 在 check 中为 hint（E-VAL-218），在 `rtsl` 中为错误。诊断信封/退出码沿用协议；成功时 stderr 记摘要，stdout 仅在 `--diagnostics json` 时输出信封。**Phase 3 衔接**：check 成功且存在未测时的 `measured by llm` 任务时，信封 `result.pending_measurements` 数组列出每个待测任务：`{task, expected_segments (= 2*lock_count+1), runs, timeout, inject: <task:<id>:time 的完整 span>}`——测时工具（tools/llm）**无需自建 RTSL 解析器**，按 §9 的整段切片替换约定，一次操作完成区间注入/artifact 更新/重新测时。
- **`ptpn rtsl FILE.rtsl`**（新子命令）：完整流水线，选项与 `tdg` 相同（`-m`、canonicalization、reduction、horizon、extrapolation、全部 export），另加 **`--export-tdg PATH`** 物化 lowering 后的 TDG JSON（供其他工具链使用）。
- **`ptpn allocate`**：输入扩展 `.rtsl` 时走 RTSL 路径（`core any` 展开后搜索）；`.json` 行为不变。
- `ptpn tdg` / `ptpn ptpn`：保持不变（C4）。

## 12. 诊断码分配（RTSL 段）

| 码 | 类别 | 触发 |
|---|---|---|
| E-SYN-101 | syntax | 词法错误：非法字符/未知单位后缀/非法转义 |
| E-SYN-102 | syntax | 语法错误：unexpected token（消息含期望关键字集合，利于 LLM 修复） |
| E-SYN-002 | syntax | 文件打不开（与 JSON 路径共用） |
| E-VAL-201 | validation | 重复 task id |
| E-VAL-202 | validation | 用户标识符以 `__` 开头或包含 `__`（保留给隐式节点，保证 §8 命名单射） |
| E-VAL-203 | validation | 重复边声明（含端点顺序不同；报两处位置） |
| E-VAL-204 | validation | 边/start/end 引用未知任务 |
| E-VAL-205 | validation | 任务属性互斥（如 periodic 带 mit / 非 periodic 带 period） |
| E-VAL-206 / 207 / 208 | validation | period / mit / deadline 非正 |
| E-VAL-209 | validation | core 越界（含 allowed_cores 集合成员） |
| E-VAL-210 | validation | allowed_cores 为空/含重复 |
| E-VAL-211 | validation | 缺少必带属性：task 缺 `time`；periodic 缺 `period`；sporadic 缺 `mit` |
| E-VAL-212 | validation | time 区间 min > max（段数约束复用 E-VAL-015 语义） |
| E-VAL-213 | validation | `measured` 缺区间（rtsl/allocate 时；fix: run measurement） |
| E-VAL-214 | validation | `estimated`/`certified` 缺区间 |
| E-VAL-215 | validation | 未知 policy（`policy` 后接 IDENT，语义层经 `parse_schedule_policy` 校验） |
| E-VAL-216 | validation | 多对多边（源与目标元素数均 ≥ 2；fix: 拆分为显式中间任务） |
| E-VAL-217 | validation | system 级必填/范围：`cpus` 缺失或 < 1、`cores_per_cpu` < 1 |
| E-VAL-218 | validation | `core any` 用于 rtsl（fix: use `ptpn allocate`） |
| E-VAL-219 | validation | 属性重复声明（同一块内 cpus/policy/period/core/time/measure 子属性等重复出现） |
| E-VAL-220 | validation | measure 配置非法：`runs`/`timeout` 非正整数 |
| E-UNS-001 | unsupported | lowering 支持类别守卫（沿用，subject 由 SourceMap 反填） |
| E-UNS-003 | unsupported | sporadic 未实现（fix: `periodic { period = mit }` 保守替代） |
| E-UNS-004 | unsupported | `us` 亚毫秒单位（fix: 使用 ms 并调整模型精度） |

退出码、优先级、10 条上限、信封格式：完全沿用 `docs/diagnostics.md`。

**复用说明**：lowering 后的图级/模型级校验（DAG 环检测、锁名前缀 E-VAL-014、锁分段数 E-VAL-015、任务 core 与 allowed_cores 一致性 E-VAL-011 等）直接复用 `docs/diagnostics.md` 的 E-VAL-0xx 错误码，并经 SourceMap 反填 `.rtsl` 行列号。

## 13. 测试要求

- **规范化命名稳定性**：上方插入/删除边后，其余隐式节点名不变（golden）；
- **panic-mode**：一个文件多个语法错误全部报出、位置正确、级联被抑制；
- **单位折算**：混合后缀/裸整数/`time_unit s`；`us` → E-UNS-004；
- **门控**：`core any` × check/rtsl/allocate 三种行为；
- **测时注入**：`.rtsl` 注入区间后 runs/timeout/artifact 保留（tools/llm 侧）；
- **SourceMap**：校验/lowering 错误反填行列；查不到键时 location 省略；
- **退出码矩阵**：每类代表性场景（沿用 test_diagnostics.cpp 模式）；
- **DSL→TDG golden**：固定示例的 lowering 结果（DOT）逐字节稳定；
- 双配置构建（compat OFF/ON）+ clang-format 21.1.8。

## 14. TODO（v2+）

1. sporadic 实现（保守 periodic-mit 建模或引擎级任意到达语义——后者违反 C1，需单独立项）；
2. interrupt 抢占语义（is_interrupt 钩子启用 + 语义文档 + 测试）；
3. 亚毫秒精度：引擎 64 位时间（违反 C1，单独立项）；
4. fork/join 显式属性（time/core/priority）与命名引用；
5. `start T { tokens N }` 多 token 支持；
6. `periodic` + `interrupt` 组合；
7. `ptpn allocate` 的 RTSL 参数化（measure 参数透传）。

## 附录 A：完整示例

```rtsl
system demo {
  cpus 4
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task sensor: periodic {
    period 100ms
    deadline 100ms
    core 0
    time [2, 3] estimated by "user spec sheet"
  }

  task fusion: aperiodic {
    core {1, 2, 3}
    locks mutex1
    time [ [1, 2], [3, 4], [1, 2] ] measured by llm { runs 20, timeout 5 }
  }

  task control: interrupt {
    core 0
    time [1, 1] estimated
  }

  task display: aperiodic {
    core 2
    time [1, 1] estimated
  }

  task log: normal {
    core any                      // 需 ptpn allocate
    time [1, 2] estimated
  }

  start sensor
  start control                 // interrupt 任务无入边，需 start 绑定才有初始 token
  end display

  sensor -> {fusion, log}         // 隐式节点 __fork_sensor__fusion__log
  {fusion, log} -> display        // 隐式节点 __join_fusion__log__display
}
```
