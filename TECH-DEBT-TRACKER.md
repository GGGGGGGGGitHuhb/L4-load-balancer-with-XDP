# 当前概况

V0.1/S1 工程骨架与配置入口已完成（Completed，2026-09-07），Reviewer 结论 PASS。S2 已完成（2026-09-08，Reviewer002 PASS），S3 已完成（Reviewer001 PASS），V0.1 开发范围完成，本地 main 已包含 S3 合并提交 544c8d8；远端发布状态本轮未核验。

- 已具备 C++20 工程、CLI、严格静态配置、固定轮询 TCP 双向转发、有界背压与半关闭，以及 Debug/Release 自动化测试。
- TD-001/TD-002 已关闭；TD-003 XDP 环境按原计划持续跟踪，TD-004 剩余规格随对应阶段产出。
- `docs/specs/config-schema.md`、`docs/specs/tcp-forwarding-semantics.md` 与 `docs/runbooks/local-dev-env.md` 已存在；benchmark 文档仍属后续阶段。
- 无本阶段阻塞或新增技术债；S2-R001 已修复并复审关闭，完成报告为 `docs/leader/reports/V0.1/S2-report-004.md`；S3 已完成且无新增债项，见 `docs/leader/reports/V0.1/S3-report-003.md`。

## 阶段状态

### V0.3 / S1 健康检查状态机

Completed（2026-09-09），V0.3-S1-D1保持Approved；Builder001、Reviewer001独立PASS、Leader003收尾完成，见 `docs/leader/reports/V0.3/S1-report-003.md`。Debug14/14、Release15/15及Production验证通过，42文件指纹一致。当前HEAD9d8310d为准备提交，7821b25为历史合并基点；本阶段尚未提交发布。TD-004健康规格义务已履行，指标规格留S2；长期债项不整体关闭。默认off、Unknown预热与UDP代理信号边界保持；重大偏差、新增技术债、阻塞和PM决策：None。

### V0.2 / S3 UDP 验证与语义文档

状态：Completed（2026-09-09），V0.2-S3-D1保持Approved，Reviewer001 PASS、Leader003收尾。S3七条AC及V0.2六条标准均满足，见 `docs/specs/v0.2-acceptance.md` 和 `docs/leader/reports/V0.2/S3-report-003.md`。独立Debug快速10/10、Release11/11及原60秒产品验证通过。无新增债项、返工、未解决发现或PM待决策；S3已合并，远端main与v0.2-s3标签核验为7821b25；当前codex/v0.3-s1的S1已Completed。

### V0.2 / S2 UDP flow table

状态：Completed（2026-09-08）；V0.2-S2-D1 保持 Approved，Reviewer001 PASS、Leader003 收尾。分支 `codex/v0.2-s2` 从 S1 合并提交 53236b1 开始，远端 main 和注释标签 v0.2-s1 已核验指向该提交。设计见 `docs/leader/designs/V0.2/S2-design.md`；自包含决策见 `docs/leader/reports/V0.2/S2-report-001.md`。

S2 的 M1..M4 与 AC-01..08 已完成，Reviewer 独立 Debug/Release 各 9/9，Production 与普通 uid 权限补测通过。TD-004 最小 UDP flow 规格义务已完成，S3完整语义/系统产品矩阵现已完成；无新增债项、未解决发现、范围偏差或 PM 待决策。批准见 Leader002，完成报告见 `docs/leader/reports/V0.2/S2-report-003.md`。

### V0.2 / S1 调度抽象与配置扩展

状态：Completed（2026-09-08）；V0.2-S1-D1 保持 Approved，Reviewer002 PASS、Leader003 收尾。设计见 `docs/leader/designs/V0.2/S1-design.md`，准备报告 001 保留；批准登记见 `docs/leader/reports/V0.2/S1-report-002.md`。

M1..M4 与 AC-01..07 已完成：独立 Debug/Release 各 7/7，Production 验证通过；F-001 Closed，无未解决事项、新增技术债或 PM 待决策。TD-004 的 scheduler 规格和配置扩展义务已完成，UDP flow 最小规格已在 S2 完成，S3完整语义现已完成；TD-003 不阻塞用户态阶段。完成报告见 `docs/leader/reports/V0.2/S1-report-003.md`；当前S2、S3及V0.2开发范围均完成。

### V0.1 / S1 工程骨架与配置入口

状态：Completed。设计与审查基线保留 Approved，批准见 Leader 报告 002。

原计划目标与实际结果：CMake 工程、CLI、配置模型、最小测试框架和配置规格均已交付，AC-01 至 AC-07 全部通过；无待完成的 S1 事项。

验证情况：独立 Debug/Release 各 CTest 2/2、357 项单元检查、22 个 CLI 用例，另有 56 项进程探针；输入 SHA256 不变，权限拒绝实际执行，Release 故意失败模式退出 1。

相关证据：

- 设计：`docs/leader/designs/V0.1/S1-design.md`
- 实现：`docs/builder/reports/V0.1/S1-report-001.md`
- 独立验收 PASS：`docs/reviewer/reports/V0.1/S1-report-001.md`
- 完成报告：`docs/leader/reports/V0.1/S1-report-003.md`

### V0.1 / S2 TCP 转发最小闭环

状态：Completed；S2-D1 保持 Approved，批准见 Leader003，收尾见 Leader004。

原计划目标与实际结果：TCP 监听、后端连接、固定轮询、双向转发、背压、半关闭、超时与信号停止、基础日志和 TCP 语义规格全部交付。

验证情况：Reviewer 独立 Debug/Release 全套各 5/5、s2 标签各 3/3；AC-01 至 AC-08 全部通过。两方向真实慢读确认背压，220 次循环 fd 回基线且 session/token 归零；实际 TCP 与确定性注入证据分开记录。

返工闭环：首轮 S2-R001（P2）发现反向半关闭测试断言传播缺失；Builder002 修复，Reviewer002 正向及八项负向验证通过并关闭。不是延期技术债，无遗留阻塞。

相关证据：

- 设计：`docs/leader/designs/V0.1/S2-design.md`
- 实现：`docs/builder/reports/V0.1/S2-report-001.md`、`S2-report-002.md`
- 审查：`docs/reviewer/reports/V0.1/S2-report-001.md`（历史 FAIL）、`S2-report-002.md`（PASS）
- 完成报告：`docs/leader/reports/V0.1/S2-report-004.md`

### V0.1 / S3 TCP 转发验证与报告

状态：Completed；S3-D1 保持 Approved，用户批准见 Leader002，独立验收 Reviewer001 PASS，收尾 Leader003。

计划与实际：新增真实产品自动验收、动态端口/有界就绪/清理、停机恢复验证、跟踪内运行手册与 V0.1 验收矩阵。S1/S2 产品行为不变，七条 V0.1 完成标准全部满足。

验证：Reviewer 独立 Debug/Release 全套各 6/6，s3 各连续 3 次及双实例并行通过；生产无测试构建通过。三类负向共 6 次退出 1 且清理正常，手动与自动路径实跑通过；精确证据见审查报告。

待处理强制事项：None。新增技术债、返工、阻塞、PM 待决策：None。本地 main 已包含 S3 合并提交 544c8d8，远端发布状态本轮未核验；V0.2/S1、S2、S3及V0.2开发范围完成；已有 TD-003/TD-004 后续阶段范围不变。

相关文档：

- 设计：`docs/leader/designs/V0.1/S3-design.md`
- 实现：`docs/builder/reports/V0.1/S3-report-001.md`
- 审查：`docs/reviewer/reports/V0.1/S3-report-001.md`
- 完成：`docs/leader/reports/V0.1/S3-report-003.md`
- 公开矩阵：`docs/specs/v0.1-acceptance.md`

## 技术债条目

### TD-001 README 构建命令尚未真实可用

状态：已解决（2026-09-07）。

影响范围：`README.md`、`V0.1 / S1`、构建与测试入口。

发现来源：初始化文档编写。

问题描述（发现时）：

`README.md` 中已经列出目标工程形态下的 CMake/Ninja 构建命令和 CTest 测试命令，但当前仓库尚未包含 `CMakeLists.txt`、源码目录和测试目标。这些命令已经标注为后续工程骨架建立后使用，但在 `V0.1 / S1` 完成前仍不能真实运行。

风险：

如果后续实现忘记同步 README，用户或新 Agent 可能复制命令后得到失败结果，误判项目不可构建或文档不可信。

处理建议：

在 `V0.1 / S1` 建立 CMake 工程骨架后立即更新 README，将目标命令改为真实可运行命令，并补充预期输出。

当前决定：

已建立真实工程并验证 README 命令。关闭依据为 Builder 报告 001 及 Reviewer 报告 001 的 AC-01/07 PASS。

相关证据：

- `README.md`
- `ROADMAP.md` 中 `V0.1 / S1 工程骨架与配置入口`

更新记录：

- `2026-09-07`：已解决；依据 `docs/reviewer/reports/V0.1/S1-report-001.md` 独立验收及 Leader 报告 003 收尾。

- `2026-05-19`：新增，原因是 README 已给出目标构建命令，但工程骨架尚未落地。

### TD-002 配置格式和测试框架选择及落地

状态：已解决（2026-09-07）。

影响范围：`V0.1 / S1`、`src/config/`、`tests/`、`docs/specs/config-schema.md`。

发现来源：架构文档和路线图初始化。

问题描述（发现时）：

项目已经确定需要静态配置文件、配置校验和自动化测试，但尚未确定配置格式和 C++ 测试框架。候选配置格式可能包括 JSON、YAML 或 TOML；测试框架可能包括 GoogleTest、Catch2 或其他轻量方案。

风险：

如果在实现中临时选择且不记录原因，后续配置兼容性、依赖管理和测试组织会变得随意，影响项目可维护性和简历叙事的一致性。

处理建议：

在 `V0.1 / S1` 阶段设计中明确选择标准，并在实现报告中记录最终选择原因。配置格式一旦进入 README 或 `docs/specs/config-schema.md`，后续变更应按兼容性原则处理。

当前决定：

2026-09-07 已确定简单 `key=value` 配置与 CTest + 独立 C++ 测试程序，不引入第三方下载依赖。理由、语法与测试契约见 `docs/leader/designs/V0.1/S1-design.md`；现已落地并由 Reviewer 报告 001 的 AC-01 至 AC-07 验收 PASS，关闭此项。

相关证据：

- `ARCHITECTURE.md` 中项目技术概览和数据模型与持久化章节。
- `ROADMAP.md` 中 `V0.1 / S1 工程骨架与配置入口`。

更新记录：

- `2026-09-07`：已解决；依据 `docs/reviewer/reports/V0.1/S1-report-001.md` 独立验收及 Leader 报告 003 收尾。

- `2026-05-19`：新增，原因是配置格式和测试框架是工程骨架阶段的关键基础选择。

### TD-003 云服务器 XDP/eBPF 验证环境尚未确认

状态：持续跟踪。

影响范围：`V1.1`、`V1.2`、XDP/eBPF 数据面、性能验证。

发现来源：用户环境讨论和架构文档初始化。

问题描述：

当前开发环境是 WSL2，适合用户态 C++ 开发、基础测试和部分 eBPF 编译学习，但不适合作为 XDP native mode 的最终性能验证环境。本机 Intel Wi-Fi 6E AX211 不适合作为 XDP 验证网卡，因此项目环境路线调整为：WSL2 负责用户态开发和基础验证，云服务器 Linux 环境负责 XDP/eBPF 功能验证和阶段收尾。

云服务器方案仍需确认具体实例规格、内核版本、虚拟网卡类型、权限能力、attach mode 和云厂商网络限制。

风险：

如果没有提前确认云服务器环境，`V1.1` 和 `V1.2` 可能只能完成编译或 generic mode 验证，无法给出可信的 XDP attach、map 交互和包级处理验证结果。即使云服务器可以完成验证，其性能结果也只能代表对应云环境，不能泛化为物理网卡 native XDP 极限性能。

处理建议：

在进入 XDP/eBPF 相关版本前，创建 `docs/runbooks/linux-xdp-env.md`，记录云服务器厂商、实例规格、内核版本、虚拟网卡、权限、工具链、attach mode 和验证命令。若云环境无法完成预期验证，应把 XDP 范围收窄为编译、加载或 map 交互验证，并明确写入技术债。

当前决定：

持续跟踪，不阻塞 `V0.1` 到 `V1.0` 的用户态版本。XDP/eBPF 阶段以云服务器作为首选收尾环境。

相关证据：

- `README.md` 环境要求。
- `ARCHITECTURE.md` XDP/eBPF 数据面层。
- `ROADMAP.md` 中 `V1.1` 和 `V1.2`。

更新记录：

- `2026-05-21`：将验证路线从“待确认 Linux 环境”收敛为“WSL2 开发，云服务器完成 XDP/eBPF 功能验证和收尾”，并补充云环境性能结果不可泛化的风险。
- `2026-05-19`：新增，原因是当前环境为 WSL2，XDP 真实验证需要后续确认。

### TD-004 长期规格文档随阶段补齐

状态：部分完成，剩余部分按原阶段接受延期。

影响范围：`docs/specs/`、`docs/runbooks/`、`docs/benchmarks/`、阶段设计与验收。

发现来源：用户关于 API 文档、数据库设计文档替代物的讨论。

问题描述：

项目已经明确后续需要网络语义、配置 schema、UDP flow table、XDP map schema、运行手册和性能方法等长期文档，S1 已创建并验证 `docs/specs/config-schema.md`，启动阶段已有 `docs/runbooks/local-dev-env.md`。S2 已补齐并验证 `docs/specs/tcp-forwarding-semantics.md`；S3 已补齐并独立验收 `docs/runbooks/local-tcp-validation.md` 和 `docs/specs/v0.1-acceptance.md`，V0.1 文档义务已完成；V0.2/S1 已补齐并独立验收 `docs/specs/scheduler.md` 和配置扩展规格；V0.2/S2 已补齐并独立验收最小 `docs/specs/udp-flow-table.md`；V0.2/S3完整UDP语义、运行手册与系统/版本矩阵已独立验收，V0.2配置与scheduler/UDP文档义务完成；V0.3/S1健康检查规格已交付并独立验收；后续指标、XDP/benchmark文档仍按原定阶段推进，不提前写未实现行为。

风险：

如果后续阶段只写代码不补充规格文档，项目会缺少高性能网络方向最重要的设计证据：数据路径、状态模型、包处理边界和性能验证方法。

处理建议：

按阶段自然生长：

- `V0.1` 创建 `docs/specs/config-schema.md` 和 `docs/specs/tcp-forwarding-semantics.md`。
- `V0.2/S1` 的 scheduler 规格和配置更新已完成；`V0.2/S2` 的最小 `docs/specs/udp-flow-table.md` 已完成，`V0.2/S3`完整语义、运行手册和系统/版本矩阵已完成。
- `V0.3/S1` 已完成并独立验收 `docs/specs/health-check.md`；`V0.3/S2` 的 `docs/specs/metrics.md` 继续按原阶段交付。
- `V0.4` 创建 `docs/benchmarks/methodology.md`。
- `V1.1` 或 `V1.2` 创建 `docs/specs/xdp-map-schema.md` 和 `docs/runbooks/linux-xdp-env.md`。

当前决定：

接受延期。Leader 在阶段设计中负责指定最小文档产出，Builder 随实现更新，Reviewer 审查一致性。

相关证据：

- `ROADMAP.md` 相关文档章节。
- `docs/leader/GUIDE.md` 规格文档规划职责。
- `docs/builder/GUIDE.md` 规格文档维护职责。
- `docs/reviewer/GUIDE.md` 规格文档审查职责。

更新记录：

- `2026-05-19`：新增，原因是长期规格文档需要随项目演进生成，当前不应一次性铺满。

## 风险观察

### OBS-001 文档先行可能导致早期文档多于实现

观察到什么：

初始化时先建立 README、架构、路线图和 Agent 规则。S1 现已实现并验收，构建命令真实可用，当前未观察到文档阻塞实现。

目前为什么还不算技术债：

这是项目初始化的预期状态。文档用于约束后续实现范围，并未阻塞开发。

什么时候需要升级为技术债：

如果 `V0.1 / S1` 完成后 README、ROADMAP、ARCHITECTURE 与实际工程结构仍明显不一致，或文档承诺的命令长期不可运行，应升级为技术债。

### OBS-002 XDP 与完整 TCP proxy 的边界需要持续防守

观察到什么：

项目目标包含 L4LB 和 XDP/eBPF，容易在后续讨论中把 XDP fast path 扩张成完整 TCP proxy。

目前为什么还不算技术债：

`ARCHITECTURE.md` 和 `ROADMAP.md` 已明确 XDP 不承载完整 TCP 代理语义。

什么时候需要升级为技术债：

如果阶段设计或实现开始把完整 TCP 连接代理逻辑塞入 XDP/eBPF，应新增技术债或要求返工。

### OBS-003 高性能目标需要可复现性能证据

观察到什么：

项目面向高性能网络方向，但当前尚未有 benchmark 方法、工具或结果。

目前为什么还不算技术债：

性能验证在 `V0.4` 才进入路线图，不要求初始化阶段完成。

什么时候需要升级为技术债：

如果 `V0.4` 后仍只有功能测试，没有可复现的吞吐、延迟或 CPU 使用率记录，应升级为技术债。

### OBS-004 L4LB 与高性能 HTTP 服务器需要保持分层差异

观察到什么：

用户已有高性能 HTTP 服务器项目，本项目用户态阶段同样会使用 C++、Linux socket、non-blocking I/O 和 `epoll`，存在部分底层能力重叠。

目前为什么还不算技术债：

`README.md` 和 `ROADMAP.md` 已明确本项目定位为传输层网络基础设施，核心差异在 TCP/UDP 四层转发、后端调度、UDP flow table、健康检查、控制面/数据面分离和 XDP/eBPF fast path。

什么时候需要升级为技术债：

如果后续实现只停留在通用 epoll 连接处理，没有落地 UDP flow table、后端调度、健康检查、控制面/数据面边界或 XDP/eBPF 原型，应升级为项目定位风险。

## 下一阶段检查点

V0.1与V0.2开发范围已完成；V0.3/S1 Completed，S2/S3未启动。以下 S1 启动检查点已完成并经阶段验收确认，仅保留作为历史记录：

- 确认阶段设计不提前实现 TCP 转发细节，除非是 CLI 或配置验证所需的最小占位。
- 确认 CMake/Ninja/LLVM 命令在当前 WSL2 环境可运行，或明确无法运行的原因。
- 确认配置格式和测试框架选择有理由，并记录到设计或报告中。
- 确认 README 中的命令在工程骨架落地后同步更新。
- 确认 `docs/specs/config-schema.md` 是否需要在本阶段创建最小版本。
- 确认生成文件、构建产物和测试临时文件不会进入源码或手写文档目录。
- 确认 `V0.1` 设计保持四层负载均衡定位，不把本项目写成另一个应用层 HTTP server。

## 关闭规则

技术债关闭时不删除条目。关闭方式：

- 将状态改为 `已解决` 或 `不再适用`。
- 在“更新记录”中说明关闭日期和依据。
- 引用 Reviewer 审查报告、测试命令、设计变更或用户明确决定。
- 如关闭后仍有残余风险，应新增风险观察或新的技术债条目。

可接受的关闭依据：

- Reviewer 审查报告确认通过。
- 测试命令和结果证明问题已修复。
- 阶段设计文档明确取消该需求。
- 用户明确接受风险或延期。

## 变更记录

- `2026-05-21`：更新 XDP/eBPF 环境技术债，明确云服务器作为 XDP 验证和收尾环境；新增 L4LB 与高性能 HTTP 服务器分层差异观察项。
- `2026-05-19`：创建初版技术债跟踪文档，记录初始化阶段的构建命令、配置选择、XDP 环境和长期规格文档风险。
