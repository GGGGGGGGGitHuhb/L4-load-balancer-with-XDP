# Changelog

本文件记录项目已经完成、发布或合并的重要变化。未来计划写入 `ROADMAP.md`，架构说明写入 `ARCHITECTURE.md`，实现细节写入 Builder 报告，审查结果写入 Reviewer 报告，技术债写入 `TECH-DEBT-TRACKER.md`。

## Unreleased

### 新增

- 新增项目入口文档 `README.md`，说明项目定位、当前状态、环境要求、快速开始占位、项目结构、测试验证策略、文档索引、开发流程和已知限制。
- 新增长期架构文档 `ARCHITECTURE.md`，明确 C++20 用户态 L4 负载均衡器、控制面、用户态数据面、XDP/eBPF 数据面和基础设施层的职责边界。
- 新增总体路线图 `ROADMAP.md`，定义从 `V0.1` 用户态 TCP 转发骨架到 `V1.2` XDP L4 fast path 原型的版本范围、阶段划分、禁止范围和完成标准。
- 新增技术债跟踪文档 `TECH-DEBT-TRACKER.md`，记录初始化阶段需要跨阶段跟踪的构建命令、配置选择、XDP 环境和长期规格文档风险。
- 新增仓库级 Agent 工作规则 `AGENTS.md`，说明文档权威来源、全局工作原则、文档语言要求和变更记录要求。
- 新增 Leader、Builder 和 Reviewer 角色规则，分别位于 `docs/leader/AGENTS.md`、`docs/builder/AGENTS.md` 和 `docs/reviewer/AGENTS.md`。
- 为三个角色补充长期规格文档职责：Leader 负责规划，Builder 负责随实现维护，Reviewer 负责审查实现、测试、报告和文档是否一致。

### 变更

- 将项目方向收敛为 C++20 用户态 TCP/UDP L4 负载均衡器优先，XDP/eBPF fast path 后续演进。
- 明确本仓库不引入 DPDK 数据面；DPDK L3 forwarding 或 NAT 可作为后续独立项目方向。
- 明确 WSL2 适合用户态开发和基础验证，但不作为 XDP native mode 性能验证环境。
- 明确当前 README 中的 CMake/Ninja/CTest 命令属于目标工程形态，需等待 `V0.1 / S1` 工程骨架建立后变为真实可运行命令。

### 修复

- 修正根 `AGENTS.md` 中角色文档路径示例，使其指向实际路径 `docs/leader/AGENTS.md`、`docs/builder/AGENTS.md` 和 `docs/reviewer/AGENTS.md`。
- 清理 `README.md` 初始化过程中的重复内容，保留单一中文版项目入口文档。

### 安全

- 当前未引入代码、网络服务、文件删除、权限修改、外部命令执行或敏感信息处理。
- 架构文档已记录后续涉及 XDP attach、网络接口修改和系统权限时必须集中封装并明确错误提示。

### 验证

- 使用 `Get-Content -Encoding UTF8 README.md` 读回确认 README 内容为单一中文版。
- 使用 `Get-Content -Encoding UTF8 ARCHITECTURE.md` 读回确认架构文档已写入并保持 UTF-8 可读。
- 使用 `Get-Content -Encoding UTF8 ROADMAP.md` 读回确认路线图已写入版本范围、阶段划分和长期演进方向。
- 使用 `Get-Content -Encoding UTF8 TECH-DEBT-TRACKER.md` 读回确认技术债条目、风险观察和下一阶段检查点已写入。
- 使用 `Get-Content -Encoding UTF8 AGENTS.md` 及三个角色 `AGENTS.md` 读回确认职责补充已写入。

### 相关文档

- 项目入口：`README.md`
- 架构文档：`ARCHITECTURE.md`
- 路线图：`ROADMAP.md`
- 技术债：`TECH-DEBT-TRACKER.md`
- 仓库规则：`AGENTS.md`
- Leader 规则：`docs/leader/AGENTS.md`
- Builder 规则：`docs/builder/AGENTS.md`
- Reviewer 规则：`docs/reviewer/AGENTS.md`
