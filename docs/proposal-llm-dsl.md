# 方案：DSL + LLM 主线化、旁路工具降级、LLM 可消费的错误处理

> 状态：v3 方案（已全部落地，见下）。评审决策与补充决定全部吸收（第 9 节），无待确认点。

## 落地状态

Phases 0–4 已全部落地到 `ptpn` 分支；本文保留为原始设计，正文不再改写。

| 阶段 | 内容 | 合入 |
|---|---|---|
| Phase 0 | Romeo/PToPNer 适配器降级为 opt-in compat | #50 |
| Phase 1 | LLM 可消费的版本化诊断协议 | #51 |
| Phase 2 | RTSL 词法 / panic-mode 语法 / 验证 + `ptpn check` | #52 |
| Phase 3 | RTSL lowering、`ptpn rtsl`、`allocate` over `.rtsl` | #53 |
| Phase 3 | 测时闭环（`tools.llm measure`）+ `rt-system` skill | #54 |
| Phase 4 | README、规格/诊断同步、CI 冒烟与示例 | 本阶段 |

剩余 TODO（sporadic、interrupt 抢占、亚毫秒精度、fork/join 属性等）见
[docs/rtsl-spec.md](rtsl-spec.md) §14。

## 1. 项目约束（本方案的前提，违反其中任何一条都应重新讨论）

- **C1 认证核心不可变**：`src/analysis/` 的 state-class 语义、canonicalization（`equality`/`max-lower`/`intersection`）、reduction（`upper-bound`/`cone`/`symmetry`）、metrics 的 `exact` 标志与 `wcrt_lb` 契约保持现状。DSL 只是**前端**，编译到 TDG 模型，绝不引入第二套分析语义。
- **C2 技术栈不变**：C++17、CMake ≥ 3.27 + Ninja、Boost（graph/filesystem/thread/date_time）、FetchContent 依赖（nlohmann/json、CLI11、spdlog/fmt、GoogleTest）。DSL 解析器**手写递归下降**（词法 + 语法 + 精确位置跟踪 + panic-mode 恢复），零新增依赖——第三方解析库的报错位置与恢复策略通常不可控，而精确的多错误报告是本方案的核心卖点之一。
- **C3 时序溯源端到端保留**：`WcetProvenance {model, certified, measured, estimated}`（`src/model/tdg.h` 已定义）。`measured ≠ certified` 的信任边界（`docs/llm-integration.md`）继续成立，`allocate --require-certified-wcet` 门控继续生效。
- **C4 向后兼容**：现有 TDG JSON 路径、`.ptpn` 路径、CLI 选项、`run.py` 的 `[STATS]` 日志契约、golden 测试全部保持工作。RTSL 是新增入口；**RTSL 路径在进程内直接构建内存 TDG，不经过 JSON 文本往返**（TDG 模型保持 JSON 可序列化、不被 SourceMap 污染）。
- **C5 LLM 是不可信提议者**：LLM 的一切输出（RTSL 文本、生成的 C 代码、测量结果）都要过验证器；生成代码只在用户显式允许时编译运行（沙箱 + 超时 + 资源限制，沿用现有原型的机制）。
- **C6 主线 CI 保持绿灯**：Romeo/PToPNer 适配从主线构建摘除，但保留一个 compile-only 防腐化作业，防止代码腐烂。
- **C7 诊断协议单一、版本化**：同一份 JSON 协议（信封带 `protocol` + `version` 字段），stdout、文件、LLM 工具返回体只是三种传输方式，schema 只有一份，进 `docs/diagnostics.md`。版本升级策略：加字段 = 小版本；改语义/删字段 = 大版本，LLM 按信封版本号选择行为。

## 2. 现状问题

1. **三套 lowering 平铺在主线**：`tdg2pn` / `tdg2romeo` / `tdg2ptopner` 同级构建、同级 CLI（`export` 子命令）、同级测试，但只有 `tdg2pn` 是分析主线，另两个只是外部工具（Romeo、PToPNer）的格式适配。
2. **错误处理是"日志文本 + 字符串 Result"**：`src/parse/json.cpp` 收集 `add_error(string)`，spdlog 打日志，`src/common/result.h` 的 `Result<T>` 只带字符串。没有错误码、分类、退出码协议、机器可读输出。LLM 无法可靠区分"语法错 / 语义错 / 性能问题 / 分析结论"。
3. **tools/llm 原型与主线脱节**：输入是自造的 spec JSON schema（`tools/llm/schema.py`），不是项目输入格式；与 CLI 没有共同入口；错误修复循环不存在。
4. **TDG JSON 是唯一输入格式**：无注释、无位置信息、provenance 字段散落，对人和 LLM 都不友好。
5. **`TaskType::INTERRUPT` 是半成品**：`src/lower/tdg2pn/preemption.cpp` 已有 `is_interrupt` 分支，但 JSON 解析器只接受 `task`/`fork`/`join`/`empty` 且永远置 `NORMAL`，`docs/json_format.md`、`docs/rule.md` 均无 interrupt 语义文档，也没有任何测试触达该分支。`sporadic`（最小到达间隔）在整个模型层不存在。

## 3. 目标架构

```
用户自然语言 ──(Skill)──► RTSL 源文件 ──ptpn check──► 诊断 JSON（一次报全文件所有错误）
                              │
                              ▼
                 进程内 lowering：RTSL → TDG 模型 + SourceMap（不经过 JSON 文本）
                              │
               测时三模式：llm-measured / user-estimated / tool-certified
                              │
                              ▼
                   ptpn analyze / allocate ──► metrics JSON + 诊断 JSON
                              │
                              ▼
              报告：WCRT、可调度性、分配结果、exact 标志、信任边界提醒
```

**主线**（默认构建、主线测试）：`src/dsl/`（新）、`src/parse/`、`src/model/`、`src/lower/tdg2pn/`、`src/analysis/`、`src/app/`、`tools/llm/`（升级）、`.claude/skills/rt-system/`（新）。

**旁路**（`-DPTPN_ENABLE_COMPAT=ON` 才构建，主线测试不跑）：`src/lower/tdg2romeo/`、`src/lower/tdg2ptopner/`、CLI 的 `export romeo|ptopner` 子命令（条件注册）。`tools/PToPNer`、`tools/romeo-2t` 保持 submodule，仅作参考，不参与构建。

## 4. RTSL 设计

### 4.1 设计原则

1. **声明式**：描述"系统是什么"，不描述"怎么跑"。
2. **三种读者**：用户手写可读、LLM 生成容易、解析器报错可定位到行列且**一次报全**（panic-mode）。
3. **与 TDG 模型 1:1**：每个构造对应现有 TDG 字段，进程内 lowering 无信息损失、无第二套语义（C1、C4）。
4. **时序来源是语法的一部分**：provenance 一等公民（C3）。
5. **确定性**：任何输出（隐式节点名、TDG 字段）只由输入拓扑决定，与文件编辑顺序无关（golden 稳定性的根基）。

### 4.2 语法草案（类 C 块）

```rtsl
system demo {
  cpus 4                       // num_cpus；合法 core id 为 0..3
  cores_per_cpu 1
  time_unit ms                // 裸整数的默认单位；默认 ms（见 4.5）
  policy fixed                 // fixed|rm|dm|pip|pcp|srp|edf|fifo|llf

  task sensor: periodic {
    period 100ms               // 带单位后缀的字面量，任何时间属性都接受
    deadline 100ms             // 可省略，默认 = period
    core 0                     // 固定核心
    time [2, 3] estimated by "user spec sheet"
  }

  task fusion: sporadic {      // v1 占位：语法保留，lowering TODO（见 4.7）
    mit 200ms                  // 最小到达间隔
    core {1, 2, 3}             // allowed_cores：静态分配域
    locks L1
    // 未测时（区间缺省）：
    time measured by llm { runs 20, timeout 5 }
    // 测完后由 skill 原位注入区间与 artifact，保留 runs/timeout：
    // time [2, 5] measured by llm { runs 20, timeout 5, artifact "hash + 编译命令快照" }
  }

  task control: interrupt {
    core 0
    time [1, 1] estimated
  }

  task log: aperiodic {
    core any                   // 语法保留；按 CLI 子命令门控（见 4.6）
    time [1, 2] estimated
  }

  start sensor                 // 可选：对应 JSON start_tasks / end_tasks
  end display

  sensor -> fusion             // 前驱依赖
  sensor -> {fusion, log}      // fork
  {fusion, log} -> display     // join
}
```

### 4.3 语义映射（与现有 TDG 字段一一对应）

| RTSL | TDG 模型 / JSON 字段 |
|---|---|
| `cpus N` / `cores_per_cpu N` | `configuration.num_cpus` / `.cores_per_cpu` |
| `time_unit U` | 仅影响裸整数的解释；lowering 后所有值统一为 ms 整数 |
| `policy P` | `configuration.policy` → `SchedulePolicy` |
| `task T: periodic { period }` | `periodic_tasks` + node type |
| `task T: aperiodic` / `interrupt` / `sporadic`（v1 占位，见 4.7）/ 省略类型 | `TaskType::{APERIOD, INTERRUPT, …}` |
| `core N` | `nodes[].core` |
| `core {a, b}` | `allowed_cores` |
| `core any` | `allowed_cores = [0..num_cpus)`（仅 `allocate` 接受） |
| `locks L` | `locks` + lock 表 |
| `time [a, b] <来源>` | `time` + `wcet_kind` / `wcet_method` / `wcet_artifact` |
| `A -> B` / `A -> {B,C}` / `{A,B} -> C` | edges + fork/join 隐式节点（**规范化命名**，见 4.4） |
| `start T` / `end T` | `start_tasks` / `end_tasks` |

### 4.4 隐式节点规范化命名（Canonical Naming）

问题：自增 ID（`fork_0`, `fork_1`）随文件上方增删边而全体改名，golden 脆弱、诊断 `subject` 不可读。

规则：

- fork 节点名 = `__fork_<src>__<t1>__<t2>…`，目标任务按名字典序排序；
- join 节点名 = `__join_<s1>__<s2>…__<dst>`，源任务按名字典序排序；
- 名称只由**拓扑端点**决定：文件其他位置的增删不影响既有名称；
- 单目标 `A -> {B}` 不生成 fork 节点（等价于普通边，lowering 折叠）；
- 防碰撞：RTSL 词法层限制任务名为标识符 `[A-Za-z_][A-Za-z0-9_]*`，并**保留 `__` 前缀给隐式节点**——用户任务名以 `__` 开头是验证错误；
- 同一条边重复声明（含端点顺序不同，如 `{fusion, log}` vs `{log, fusion}`）是验证错误 E-VAL duplicate-edge，报出两处位置；因此规范化名天然唯一；
- 每个隐式节点在 SourceMap 中绑定到**对应边声明所在的行**（见 5.7），诊断指向用户写的边，而不是生成的节点。

### 4.5 时间单位（Lexer 原生支持）

问题：自然语言需求混用单位（"周期 10ms，中断处理 50us"），裸整数逼 LLM 心算换算，极易出错。

规则：

- **字面量后缀**：所有时间属性（`time` 区间、`period`、`deadline`、`mit`）接受 `10ms` / `1s` 后缀，词法层直接折算为 ms 整数；
- **默认单位**：`system` 块声明 `time_unit ms|s`（默认 `ms`），裸整数按此单位折算；带后缀的值永远绝对，不受影响；
- 内部统一基准为 **ms**（`INF_TIME = int::max` 约束下约 24.8 天量程，与现有整数时间模型一致）；
- **`us` 后缀 v1 不支持**：低于基准分辨率的字面量（如 `50us`）报 `unsupported` 诊断并说明原因——对认证工具而言静默舍入不可接受；亚毫秒精度建模需要引擎 64 位时间（违反 C1），记入 `docs/rtsl-spec.md` 的 TODO；
- TDG JSON 路径不受影响（C4）——其整数按现状解释为引擎 tick；
- `timeout`（测时配置）是**墙钟秒**，不是模型时间，文档中明确区分；
- metrics 输出保持引擎 tick 不变（C4）；RTSL 输入时 tick = ms，写入 `docs/rtsl-spec.md`，由 skill 在人类可读报告里做单位渲染。

### 4.6 `core any` 门控（按 CLI 子命令）

- `ptpn check`：接受，附 `hint` 级诊断"该模型含 `core any`，需用 `ptpn allocate`"；
- `ptpn analyze`：`core any` → 验证错误，`fix` = "use `ptpn allocate`"；
- `ptpn allocate`：展开为 `allowed_cores = [0..num_cpus)`，搜索 + 精确分析评估，返回 Pareto 分配。

### 4.7 sporadic 与 interrupt（决策 4 的落地方式）

**sporadic（v1 占位，实现排入 TODO）**：

- 词法/语法层**保留** `sporadic { mit }` 声明（语法冻结，后续实现不破坏已有模型文件）；
- lowering 未实现：`ptpn check` 对 sporadic 任务发 `unsupported` 诊断（E-UNS），`fix` 提示临时替代方案——"用 `periodic { period = mit }` 做保守建模（最密到达模式，WCRT 合法上界）"；
- 实现时沿用该保守建模方向，或做引擎级任意到达语义（违反 C1，需单独立项）；记入 `docs/rtsl-spec.md` 的 TODO 段。

**interrupt（v1 补齐半成品）**：

- RTSL 直接置 `TaskType::INTERRUPT`（进程内 lowering，不经 JSON，无 C4 障碍）；
- 但现状是：`preemption.cpp` 有 `is_interrupt` 分支，**无任何输入路径、无文档、无测试**。Phase 2 必须先冻结 interrupt 语义（抢占时机、是否可被挂起、与锁协议的交互），在 `docs/rule.md` 补文档，并给该分支补测试。这是一个真实的 scope 项，不是语法糖。

### 4.8 时序三模式（用户选择，在 DSL 里显式写出）

| 模式 | 语法 | `wcet_kind` | 数据谁产生 |
|---|---|---|---|
| **LLM 测时** | `time [a, b]? measured by llm { runs N, timeout T, artifact? }` | `measured` | LLM 按任务生成 C → 编译 → 沙箱运行 N 次 → skill 把 `[min, max]` 与 `artifact`（二进制哈希 + 编译命令快照）**原位注入 .rtsl**；`runs`/`timeout` 保留。显式标注非认证 |
| **用户预估** | `time [2, 3] estimated by "note"` | `estimated` | 用户直接写区间 |
| **工具认证** | `time [a, b] certified by "aiT" { artifact "..." }` | `certified` | 外部静态 WCET 工具 / MBPTA 的 artifact 导入 |

`measured` 的区间是**可选**的：未测时缺省，`ptpn check` 接受；`ptpn analyze` 对缺区间的 `measured` 报验证错误，`fix` = "run the measurement step first"。`.rtsl` 是唯一事实源：测完只改 `.rtsl` 一处，TDG JSON 始终是派生产物。

## 5. 错误处理：LLM 可消费的诊断协议

### 5.1 诊断分类（taxonomy）

| 类别 | 含义 | 谁负责修 | LLM 收到后的动作 |
|---|---|---|---|
| `syntax` | 词法/语法错误（含精确行列） | LLM | 按 `location` + `message` 重写 DSL |
| `validation` | 语义校验失败：重复 ID、非法区间、未知锁、deadline > period、core 越界、`__` 前缀、重复边等 | LLM | 按 `code` + `fix` 建议修正 |
| `unsupported` | 模型超出支持类别（如 EDF/FIFO/LLF 下用了锁、fork/join 或依赖驱动释放） | LLM | 换策略或简化模型 |
| `performance` | 状态爆炸：达到 `-m` 截断、horizon 剪枝、提示可用的 reduction | LLM | 加 `-m`、启用 reduction、简化模型后重跑 |
| `analysis` | 分析**结论**：deadlock、`wcrt_lb > deadline`（不可调度）。不是"错误"，是结果 | LLM | 改设计（调优先级/迁移核心/改周期）后重跑 |
| `internal` | 程序缺陷（不该发生） | 开发者 | 报告 bug |

### 5.2 panic-mode 解析恢复（一次报全文件所有错误）

问题：遇第一个语法错误就返回，LLM 每次 `ptpn check` 只能修一个标点，交互轮数爆炸。

设计：

- **恢复集**：`task {...}` 内部报错后，跳到下一个属性关键字（`period` / `deadline` / `core` / `locks` / `time` / `mit` / `}`）或换行继续；顶层报错后，跳到下一个 `task` / `start` / `end` / `}` 继续。
- **词法层不致命失败**：非法字符 → 错误 token + 诊断，继续扫描（否则恢复无从谈起）。
- **级联抑制**：解析失败的构造标记为 tainted，校验阶段跳过**依赖该构造**的检查；与其无关的校验错误照常报告。这保证一次 `check` 报出的是"真实错误全集"而不是噪声瀑布。
- **上限**：单次运行最多报 N（默认 10）条诊断，超出时信封带 `truncated_diagnostics: true`，LLM 修完再跑。
- 预期实现量：手写递归下降里约几十行（每个块一个 `synchronize(recovery_set)`）。

### 5.3 诊断 JSON 协议（单一 schema，版本化；C7）

```json
{
  "protocol": "ptpn-diagnostics",
  "version": 1,
  "ok": false,
  "exit_code": 3,
  "diagnostics": [
    {
      "code": "E-VAL-014",
      "category": "validation",
      "severity": "error",
      "location": { "file": "demo.rtsl", "line": 7, "column": 3, "span": 9 },
      "subject": "task:fusion",
      "message": "deadline 120 exceeds period 100",
      "fix": "set deadline <= period, or drop it to inherit the period"
    }
  ],
  "result": { }
}
```

- `code` 稳定进 `docs/diagnostics.md`（LLM 可记忆）；`message` 给人读；`fix` 是给 LLM 的行动建议。
- `subject` 是结构化键（`task:fusion`、`lock:L1`、`edge:sensor->fusion`、`policy`、`fork:__fork_sensor__fusion_log`…），配合 SourceMap 反填 `location`；查不到（如 JSON 输入路径）时 `location` 省略，协议允许。
- 结果类诊断（deadlock / 不可调度 / 截断）进 `result` 段，与输入问题区分。
- **传输方式**：stdout（`--diagnostics json`）、`--diagnostics-file PATH`、LLM 工具返回体——同一个信封对象，schema 唯一。

### 5.4 退出码协议（含多类别共现的优先级）

| 退出码 | 含义 |
|---|---|
| 0 | 成功（含"可调度"结论） |
| 1 | CLI 用法错误（CLI11 层） |
| 2 | 语法错误 |
| 3 | 语义验证失败 |
| 4 | 模型超出支持类别 |
| 5 | 状态爆炸/截断（已给出部分结果） |
| 6 | 分析结论为负（deadlock / 不可调度；结论在 JSON 里） |

**共现时取最高优先级类别**，全局顺序：

```
2 (syntax) > 3 (validation) > 4 (unsupported) > 6 (analysis) > 5 (performance) > 0
```

分析阶段的专门规则（防 LLM 在已证不可调度的模型上盲目调大 `-m`）：

- 截断前缀中**已发现确凿反例** → 返回 6：deadlock 状态类是真实可达状态，`wcrt_lb > deadline` 是真实下界反例——两者都不因截断而失效；
- 仅当**未发现反例**、且因截断导致 `exact = false` 无法证明安全时，才返回 5；
- 对称地：`wcrt`（上界）超过 deadline **不**构成反例，不触发 6。
- 无论退出码是什么，JSON 里始终带**全量**诊断与 `result`；退出码只是优先级摘要。`performance` 类诊断的 `fix` 文本按是否有反例区分："no counterexample found; raising -m may help" vs 设计改动建议。

### 5.5 流分离

- **stderr**：人读日志（spdlog，保持 `[STATS]` 格式不动——`run.py` 的 regex 契约，见 CLAUDE.md）；
- **stdout + `--diagnostics json`**：只输出诊断信封，作为 LLM 工具调用的返回体；
- **`--diagnostics-file PATH`**：同一信封写文件。

### 5.6 SourceMap：跨阶段保留行列号

问题：validation（环路检测、锁冲突）与 unsupported（EDF 下用锁/fork-join）往往在 TDG 构建后、甚至 tdg2pn 阶段才触发；若 RTSL 先转 JSON 文本再交给后端，行列信息丢失。

设计：

- **进程内直通**：RTSL 解析器直接构建内存 `model::TDG`（复用 `tdg.load_from_parser` 模式），**不经过 JSON 文本往返**（C4：TDG 对象本身不被 SourceMap 污染，JSON 导出照旧）。
- **SourceMap 是旁挂字典**：`dsl::SourceMap`，键 = subject 结构化键，值 = `SourceSpan {file, line, column, span}`。键族：`task:<id>`、`task:<id>:core`、`task:<id>:time`、`task:<id>:period`、`task:<id>:deadline`、`lock:<name>`、`edge:<src>-><dst>`、`fork:<name>` / `join:<name>`（绑到**边声明行**，见 4.4）、`policy`、`system:time_unit`、`start:<t>`、`end:<t>`。
- **发射时反填**：任何阶段（解析器、校验器、tdg2pn 支持类别守卫、分析器）产出的 Diagnostic 只带 `subject`；发射器查表填 `location`。tdg2pn 的守卫错误原本按 task id / lock 名 / policy 报告，天然有 subject 可查。
- 查不到键（如 JSON 输入路径、或跨模块内部错误）→ `location` 省略，协议允许。

### 5.7 实施落点

- 新增 `src/common/diagnostics.{h,cpp}`：`Diagnostic`、`Diagnostics`（收集器 + 级联抑制标记）、信封序列化（带 `protocol`/`version`）、退出码优先级计算。`Result<T>` 保持不动（小函数内部继续用）。
- `parse/json.cpp` 现有约 40 条 `add_error` 一次性分配稳定 code；`.ptpn` 解析器同样接入。
- `main.cpp` 顶层统一 try/catch：异常 → 诊断信封 + 退出码，不再裸输出堆栈/文本。
- 错误码分配表 + 退出码优先级 + 协议版本写入 `docs/diagnostics.md`。

## 6. Skill 设计（`.claude/skills/rt-system/SKILL.md`）

工作流（每个箭头后都可能回到修复循环）：

1. **需求澄清**：从用户自然语言中抽取任务、类型、依赖、核心约束；时序来源让用户选择（三模式），或用户说"全部 llm 测"；单位由 DSL 层兜底（4.5），skill 不再要求 LLM 换算。
2. **生成 RTSL** → 写文件 → `ptpn check --diagnostics json`（一次报全，panic-mode 保证每轮修复多项）。
3. **按类别修复**（动作表按 5.1 分类 + 5.4 退出码驱动）：
   - `syntax`/`validation`/`unsupported` → 改 DSL 重跑 check；
   - `performance` 且无反例 → 调整 `-m`/reduction，并向用户报告状态爆炸原因；
   - `analysis`（退出码 6，含截断下发现的死锁/下界反例）→ **不得**调大 `-m` 掩盖，向用户解释负结论，提议设计改动。
4. **测时**：对 `measured by llm` 的任务逐个走 tools/llm 测时管线（生成 C → 编译 → `runs` 次 → `timeout` 秒 → 把 `[min, max]` + `artifact` 原位注入 `.rtsl`，保留 `runs`/`timeout`，见 4.8）。
5. **分析**：`ptpn analyze`（有 `core any` 时用 `ptpn allocate`）。
6. **报告**：人读总结 + metrics JSON；必须带 `exact` 标志与信任边界提醒（measured 非认证，不能用于硬实时声明；sporadic 的保守建模说明）。

Skill 文档中写死"错误码 → 动作"映射表与退出码优先级（5.4），LLM 按表执行而不是自由发挥。

## 7. 测试策略

| 层级 | 内容 | 状态 |
|---|---|---|
| 主线 `ptpn_test` | DSL 词法/语法/验证/lowering 的 golden 测试；**规范化命名稳定性测试**（上方插入/删除边后，其余隐式节点名不变）；**panic-mode 测试**（一个文件多个语法错误，断言全部报出且位置正确、级联被抑制）；**退出码优先级测试**（截断 + 死锁 → 6；仅截断 → 5）；**单位折算测试**（混合后缀/裸整数/time_unit）；诊断协议矩阵测试（每个 code 至少一条用例，断言 code、category、location、退出码）；现有 analysis/metrics/policy 测试全部保留 | 新增 + 不变 |
| compat `ptpn_compat_test`（可选构建） | `test_tdg2romeo`、`test_conversion` 中 romeo/ppn 部分、`.cts`/`.ppn` golden | 移出主线（现有测试本就是纯 writer 测试、不执行外部工具，拆分只影响构建耦合） |
| `tools/llm/tests` | unittest 扩展：RTSL 校验、诊断信封解析、三模式测时、`.rtsl` 原位注入（区间注入后 runs/timeout 保留；artifact = 哈希 + 编译命令快照） | 扩展 |
| Skill 冒烟 | 一个端到端 example（`demo.rtsl` → check → 测时注入 → analyze）作为脚本 | 新增 |

## 8. 迁移步骤（每步独立可合入、可回滚）

- **Phase 0 — 旁路降级（约 1 天）**：CMake `PTPN_ENABLE_COMPAT`（默认 OFF）包住 `ptpn_romeo`/`ptpn_ptopner`/`ptpn_convert` 中相关部分；`main.cpp` 的 `export` 子命令与 `pipeline.cpp` 的 `--romeo/--ppn` 标志条件编译（注意 `pipeline.cpp` 被编进 `ptpn_test`，需要同样的宏）；`test/CMakeLists.txt` 拆分；CI 主线去掉 romeo/ppn 内容，新增 compile-only 防腐化作业。纯机械改动，不动语义。
- **Phase 1 — 诊断基础设施（2–3 天）**：`diagnostics.{h,cpp}` + 信封（协议版本）+ 退出码优先级；现有 JSON/`.ptpn` 路径接入；`docs/diagnostics.md` 错误码表。此阶段独立有价值，不依赖 DSL。
- **Phase 2 — RTSL（1.5–2.5 周）**：`docs/rtsl-spec.md` 设计冻结 → 词法（单位后缀）/语法（panic-mode）→ 语义验证（含级联抑制）→ 进程内 lowering + SourceMap + 规范化命名 → `ptpn check` 子命令 → golden + 稳定性测试；**含 interrupt 语义冻结与 preemption 路径补测试、sporadic 保守建模落地**。
- **Phase 3 — LLM 集成（约 1 周）**：tools/llm 改吃 RTSL + 三模式测时 + `.rtsl` 原位注入 + 诊断信封回传；`rt-system` skill 落地。
- **Phase 4 — 文档/示例（2–3 天）**：README Input Modes 表加 RTSL 行、端到端示例、迁移现有 example 说明。

## 9. 已拍板的决策

1. DSL 命名：**RTSL**（扩展名 `.rtsl`，规格 `docs/rtsl-spec.md`）。
2. 语法风格：**类 C 块**（`system {}` / `task T: kind {}` / 行内属性 / `;` 可选）。
3. `core any`：**语法保留，按 CLI 子命令门控**（check 提示 / analyze 拒绝 / allocate 展开，见 4.6）。
4. 任务类型范围：**包含 interrupt 与 sporadic**；interrupt 补全半成品路径（Phase 2），sporadic v1 语法占位 + unsupported 诊断、实现 TODO（见 4.7）。
5. 诊断协议：**同一份版本化 JSON 信封**，stdout / 文件 / 工具返回体仅为传输方式（C7）。
6. 时间基准：**默认 ms**，后缀 `ms|s`；`us` 及亚毫秒精度 v1 不支持（报 unsupported 诊断），64 位时间排入 v2 TODO（见 4.5）。
7. 诊断上限：单次最多 **10** 条。
8. `measured` artifact 内容：**二进制哈希 + 编译命令快照**。
