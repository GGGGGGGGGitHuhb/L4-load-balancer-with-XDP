# Changelog

本文件记录项目已经完成、发布或合并的重要变化。未来计划写入 `ROADMAP.md`，架构说明写入 `ARCHITECTURE.md`，实现细节写入 Builder 报告，审查结果写入 Reviewer 报告，技术债写入 `TECH-DEBT-TRACKER.md`。

## Unreleased

### 2026-09-08 V0.2/S2 完成收尾

- Reviewer001 PASS 后，Leader 核验 D1 批准、31 文件指纹、独立 9/9 及清理证据，完成阶段/路线图/债项同步；S2 Completed，D1 保持 Approved。见 `docs/leader/reports/V0.2/S2-report-003.md`。
- TD-004 的最小 UDP flow 规格义务完成；无返工、新增技术债或未解决发现。S3 完整语义/产品矩阵未开始，V0.2 整体未完成；本阶段未提交发布。以下交接记录保留当时状态。

### 2026-09-08 V0.2/S2 独立审查

- Reviewer 独立最终源码 Debug/Release 全套各 9/9（14.07/14.54 秒），普通用户纯测试各 3/3；Production Release 无测试构建、24 项 CLI 和真实 UDP 冒烟通过。网络验证使用临时 user/net namespace 的独立 lo，未改变宿主网络。
- 真实 ICMP、wildcard 源地址、零长/65507/截断及实际 fd 复用验证通过；错误回包、零长误作 EOF、错误选择三类实际产品 Release 临时突变均在目标断言退出 1，54 条产品子 PID 全部回收。
- 独立结论 PASS，无新增阻塞或技术债；见 `docs/reviewer/reports/V0.2/S2-report-001.md`。当前 Closing，交 Leader 最终同步；S3 尚未开始，未提交或发布。

### 2026-09-08 V0.2/S2 实现与 Builder 自测

- 新增独立 UDP reactor、IP_PKTINFO 实际目标 flow key 和回复源 IP、每 flow 已连接后端 socket；复用统一 Scheduler，无新配置键或 TCP reactor 变化。
- 明确零长/65507/截断、立即发送整包丢弃、60 秒空闲、1024 容量、错误隔离及 token 生命周期，新增最小 UDP flow 规格和必要测试。
- Builder Debug/Release 全套各 9/9（最终 14.10/14.39 秒）；Production Release 构建、CLI 及真实 UDP 冒烟通过；三个 Release 负向模型准确失败并清理。大包测试在临时网络命名空间独立 lo 完成，保留外部 loopback0 路由丢弃大包的环境证据，未改宿主网络。
- 当前 Ready for Review，待独立 Reviewer 和 Leader 收尾；S3 尚未开展，未提交或发布。证据见 `docs/builder/reports/V0.2/S2-report-001.md`。

### 2026-09-08 V0.2/S2 准备与 S1 标签核验

- S1 已合并至 53236b1，远端 main 与注释标签 v0.2-s1 的 peeled commit 一致；用户已完成标签推送，未据此声称创建 GitHub Release。下方阶段未提交/发布措辞保留其当时事实。
- 基于该提交切换 codex/v0.2-s2，形成 V0.2-S2-D1 Draft 设计、审查计划和准备报告，Awaiting PM Decision；UDP 实现未开始，无提交推送。已核对 16 个 S1 源码指纹一致，无需重复历史回归。

### 2026-09-08 V0.2/S1 完成收尾

- Reviewer002 PASS、F-001 Closed 后，Leader 核验源码指纹与验收证据，完成阶段、路线图及技术债状态同步；V0.2/S1 Completed，基线 D1 保持 Approved。见 `docs/leader/reports/V0.2/S1-report-003.md`。
- TD-004 的 scheduler 和配置扩展义务完成，UDP 等后续规格继续按原阶段推进；无新增债项或未解决发现。S2/S3 未开始；本阶段未提交、推送或发布。下方记录保留各次交接当时状态。

### 2026-09-08 V0.2/S1 文档修正与独立复审

- 通用 TCP 运行手册当前全套预期已更新为 7/7，说明新增 v02_scheduler_unit，并保留 V0.1/S3 历史记录；F-001 关闭。
- Reviewer002 复核源码指纹与七项测试注册一致，复用首轮独立产品证据，结论 PASS，无新增技术债。见 `docs/reviewer/reports/V0.2/S1-report-002.md`；当前 Closing，交 Leader 最终同步。

### 2026-09-08 V0.2/S1 独立首轮审查

- Reviewer 独立 Debug/Release 全套各 7/7（4.83/4.64 秒），Production Release 无测试构建与 25 项 CLI 检查通过；Release 错误 XOR 及健康后端代替失败后端的临时副本均准确退出 1。
- 首轮结论 FAIL，仅 F-001：通用 TCP 运行手册全套预期仍为 6/6，需 Builder 最小文档修正；产品及测试无需返工。见 `docs/reviewer/reports/V0.2/S1-report-001.md`。当前 Reworking，尚未完成 Leader 收尾。

### 2026-09-08 V0.2/S1 实现与 Builder 自测

- 新增可选 `protocol`/`scheduler` 严格配置及兼容默认值、统一 Scheduler/工厂和独立 round-robin；TCP 已接入，失败仍消耗一次选择且不重试。
- UDP 可静态校验，运行在任何网络资源创建前明确拒绝；同步配置、调度规格和帮助说明，未实现 UDP 数据面。
- Builder 独立 Debug/Release 全套各 7/7（最终 4.66/4.49 秒）；Release 无测试构建与完整 CLI 验证通过。实际字节验证默认/显式 A/B/A/B、失败/B/失败/B及占用端口 UDP 拒绝，Release 故意失败检查均退出 1。
- 当前 Ready for Review，尚待独立 Reviewer 验收与 Leader 收尾；报告 `docs/builder/reports/V0.2/S1-report-001.md`。未提交或发布。

### 2026-09-08 V0.2/S1 准备记录

- 从本地 main 的 S3 合并提交 `544c8d8` 创建并切换到 `v0.2-s1`，形成设计草案、审查计划及准备报告，当前为 Draft，未实现新功能。
- 新目录 Debug 构建通过，现有 CTest 6/6 通过（4.66 秒）；首次沙箱 socket 权限失败另存证据，可用环境复测通过。
- 本地提交图已证明 S3 合并；下方 S3 收尾时的未合并说明属于历史状态。远端查询因本机代理不可连接失败，本轮未核验远端发布状态。

### 2026-09-08 S3 实现与独立审查

- 新增真实产品 TCP 验收入口、动态端口演示后端、独立证据目录和有界清理，覆盖二进制轮询、1 MiB + 17 字节半关闭、后端停机恢复及启动失败；没有改变 S1/S2 产品契约。
- 新增可跟踪本地运行手册与 V0.1 验收矩阵；README 手动和自动路径均实跑通过。
- Reviewer 独立 Debug/Release 全套各6/6，s3各连续3次及双实例并行通过；生产无测试构建通过。错误数据/fixture提前退出/缺ready三类共6次准确退出1并回收，另缺换行ready、错误路径与用法检查通过。
- 独立审查 PASS，无新增阻塞或技术债；见 `docs/reviewer/reports/V0.1/S3-report-001.md`。Leader 已核对全部七条版本完成标准并完成收尾：S3 与 V0.1 开发范围 Completed，见 `docs/leader/reports/V0.1/S3-report-003.md`。尚未发布，S3 未提交、推送、合并或打标签。


### 2026-09-08 S3 开发准备

- 核验远端v0.1-s2标签并从S2合并后的main创建codex/v0.1-s3。
- 补齐S3-D1设计、审查计划及准备报告，聚焦真实产品进程验证与V0.1验收，准备当时状态为 Draft（后续批准和完成见上条）。
- 独立目录Debug构建和既有回归5/5通过；这是准备当时的前置记录，彼时 S3 尚未实现。


### 2026-09-08 S2-R001 测试返工与独立复审

- 修复反向半关闭后端断言未传播的假阳性：父测试通过独立管道在2秒内确认后端完整字节校验成功；失败消息、异常退出及未完成均失败。
- Debug/Release全套各5/5、s2标签各3/3；临时强制退出4、错误预期和后端异常三个负向副本均按预期退出1并明确诊断。只修复测试及交接文档，产品契约未变；详见 `docs/builder/reports/V0.1/S2-report-002.md`。Reviewer独立复审PASS：Debug/Release全套各5/5、标签各3/3；退出/错误数据/异常/缺消息四类负向副本共八次均准确退出1，S2-R001关闭。见 `docs/reviewer/reports/V0.1/S2-report-002.md`，Leader已收尾，S2 Completed；见 `docs/leader/reports/V0.1/S2-report-004.md`。

### 2026-09-08 S2 实现与首轮审查（历史，返工已关闭）

- 新增 `--run`、固定顺序轮询和单线程 LT epoll 双向 TCP 转发；非阻塞 connect 查询 SO_ERROR，单会话错误不终止服务。
- 新增每方向64KiB队列、背压恢复、排空后半关闭、5秒连接/60秒空闲截止、1024会话上限、token隔离及信号退出；停止不保证在途字节排空。
- 新增真实TCP集成、定向HUP/旧token状态验证、TCP语义规格及前台echo演示fixture。Builder Debug/Release各5/5、s2 label复跑3/3和README冒烟通过；逐项证据见 `docs/builder/reports/V0.1/S2-report-001.md`。Reviewer独立Debug/Release各5/5、s2 3/3及补充产品探针通过，但故意失败的reverse-fin后端断言仍导致总测试PASS；首轮审查FAIL，当时 S2-R001 要求 Builder 修复断言传播后复审；现已由上方复审记录关闭。详见 `docs/reviewer/reports/V0.1/S2-report-001.md`；不代表S2验收完成或V0.1整体完成。

### 2026-09-07 S2 设计准备

- 新增 S2-D1 详细设计、审查计划和准备报告，明确 TCP 转发、背压、半关闭与核心验证边界；当时状态为 Draft，待开发批准；2026-09-08 已批准并完成，保留准备历史。
- 复查 S1 CTest 2/2通过、本机epoll及TCP半关闭可用；当时尚未实现或验收S2。
- 更新 README 与阶段状态入口，详见 `docs/leader/reports/V0.1/S2-report-001.md`。

### 2026-09-07 S1 实现与独立验收

- 新增无第三方依赖的 C++20 CMake/Ninja 工程和 `l4lb` CLI；只检查配置，尚不转发流量。
- 新增严格 IPv4 TCP 配置解析、首错行诊断、有界只读加载和示例配置；拒绝 FIFO/设备/目录，允许普通文件符号链接。
- 新增配置规格、真实 README 命令及 Debug/Release CTest 基线：各 2 个测试通过，含 357 项单元检查和 22 个 CLI 用例。
- 实际验证普通用户权限错误、配置 SHA256 不变和 Release 故意失败返回 1；原始证据及限制见 `docs/builder/reports/V0.1/S1-report-001.md`。独立验收记录见下一条。

- Reviewer 独立干净 Debug/Release 构建均通过：各 CTest 2/2、357 项单元检查、22 个 CLI 用例；另有 56 项进程探针通过，首错、编码、大小及非阻塞文件路径符合 S1 契约。验收 PASS，Leader 已完成阶段收尾（Completed，见 `docs/leader/reports/V0.1/S1-report-003.md`）；仅 S1 完成，V0.1 尚未整体完成，详见 `docs/reviewer/reports/V0.1/S1-report-001.md`。

### 2026-09-07 启动准备

- 补齐 V0.1/S1 详细设计与审查计划，明确配置、CLI、测试基线及验收标准。
- 修正角色指南路径引用为实际的 `GUIDE.md`，补充 README 当前阶段入口。
- 增加 CMake 构建目录和测试临时文件忽略规则，保留原有本地协作文档忽略规则。
- 验证临时 C++20 工程配置、编译、CTest、epoll 和本机 TCP 双向回环通信；项目实现及项目测试尚未开始。
- 详细结果见 `docs/leader/reports/V0.1/S1-report-001.md`。

以下通用条目保留初始化时的历史记录；当前实现状态以上述 S2 完成及 S1 条目为准。

### 新增

- 新增项目入口文档 `README.md`，说明项目定位、当前状态、环境要求、快速开始占位、项目结构、测试验证策略、文档索引、开发流程和已知限制。
- 新增长期架构文档 `ARCHITECTURE.md`，明确 C++20 用户态 L4 负载均衡器、控制面、用户态数据面、XDP/eBPF 数据面和基础设施层的职责边界。
- 新增总体路线图 `ROADMAP.md`，定义从 `V0.1` 用户态 TCP 转发骨架到 `V1.2` XDP L4 fast path 原型的版本范围、阶段划分、禁止范围和完成标准。
- 新增技术债跟踪文档 `TECH-DEBT-TRACKER.md`，记录初始化阶段需要跨阶段跟踪的构建命令、配置选择、XDP 环境和长期规格文档风险。
- 新增仓库级 Agent 工作规则 `AGENTS.md`，说明文档权威来源、全局工作原则、文档语言要求和变更记录要求。
- 新增 Leader、Builder 和 Reviewer 角色规则，分别位于 `docs/leader/GUIDE.md`、`docs/builder/GUIDE.md` 和 `docs/reviewer/GUIDE.md`。
- 为三个角色补充长期规格文档职责：Leader 负责规划，Builder 负责随实现维护，Reviewer 负责审查实现、测试、报告和文档是否一致。

### 变更

- 将项目方向收敛为 C++20 用户态 TCP/UDP L4 负载均衡器优先，XDP/eBPF fast path 后续演进。
- 明确项目与高性能 HTTP 服务器的分层区别：HTTP 服务器聚焦应用层协议处理，本项目聚焦传输层 TCP/UDP 四层转发、后端调度、UDP flow table、健康检查和 XDP/eBPF fast path。
- 明确本仓库不引入 DPDK 数据面；DPDK L3 forwarding 或 NAT 可作为后续独立项目方向。
- 明确 WSL2 适合用户态开发和基础验证，XDP/eBPF 阶段优先使用云服务器 Linux 环境进行功能验证和收尾。
- 明确云服务器性能结果只代表对应云环境，不泛化为物理网卡 native XDP 极限性能。
- 明确当前 README 中的 CMake/Ninja/CTest 命令属于目标工程形态，需等待 `V0.1 / S1` 工程骨架建立后变为真实可运行命令。

### 修复

- 修正根 `AGENTS.md` 中角色文档路径示例，使其指向实际路径 `docs/leader/GUIDE.md`、`docs/builder/GUIDE.md` 和 `docs/reviewer/GUIDE.md`。
- 清理 `README.md` 初始化过程中的重复内容，保留单一中文版项目入口文档。

### 安全

- 当前未引入代码、网络服务、文件删除、权限修改、外部命令执行或敏感信息处理。
- 架构文档已记录后续涉及 XDP attach、网络接口修改和系统权限时必须集中封装并明确错误提示。

### 验证

- 使用 `Get-Content -Encoding UTF8 README.md` 读回确认 README 内容为单一中文版。
- 使用 `Get-Content -Encoding UTF8 ARCHITECTURE.md` 读回确认架构文档已写入并保持 UTF-8 可读。
- 使用 `Get-Content -Encoding UTF8 ROADMAP.md` 读回确认路线图已写入版本范围、阶段划分和长期演进方向。
- 使用 `Get-Content -Encoding UTF8 TECH-DEBT-TRACKER.md` 读回确认技术债条目、风险观察和下一阶段检查点已写入。
- 使用 `Get-Content -Encoding UTF8 AGENTS.md` 及三个角色 `GUIDE.md` 读回确认职责补充已写入。

### 相关文档

- 项目入口：`README.md`
- 架构文档：`ARCHITECTURE.md`
- 路线图：`ROADMAP.md`
- 技术债：`TECH-DEBT-TRACKER.md`
- 仓库规则：`AGENTS.md`
- Leader 规则：`docs/leader/GUIDE.md`
- Builder 规则：`docs/builder/GUIDE.md`
- Reviewer 规则：`docs/reviewer/GUIDE.md`
