# 项目概述

本项目是一个面向高性能网络方向的四层负载均衡器实验项目。它以 C++20 用户态 TCP/UDP 负载均衡器为基础，逐步建立配置、调度、健康检查、连接与会话管理、可观测性和性能验证能力；在用户态语义稳定后，再引入 XDP/eBPF fast path，探索 Linux 网络栈中的包级高性能数据面。

最终目标是形成一个可以讲清楚、可以运行、可以测试、可以观察性能瓶颈的 L4 load balancer，而不是只完成一次性 demo。项目同时服务于高性能网络方向的学习、简历展示和后续 DPDK/L3 forwarding 等独立项目的能力准备。

当前项目处于初始化阶段，优先完成文档、架构边界和 C++ 工程骨架。总体技术方向如下：

- C++20 优先。
- Linux/WSL2 开发优先。
- LLVM + CMake + Ninja 构建优先。
- 用户态 socket/epoll 路径优先。
- 用户可见文档使用中文，命令、路径和代码标识符保留原文。
- 默认测试不依赖 root 权限、真实网卡或 XDP native mode。

## 范围边界

长期范围：

- 用户态 TCP 四层转发。
- 用户态 UDP 转发和短期 flow table。
- 静态配置文件和配置校验。
- 后端调度策略。
- 后端健康检查。
- 连接、会话和后端状态指标。
- 本地自动化测试、集成测试和冒烟验证。
- 性能测试方法和基准结果记录。
- XDP/eBPF 数据面原型。
- 用户态控制面与 eBPF maps 的协作边界。

近期版本不会做：

- 完整 Layer 7 proxy。
- HTTP-aware routing。
- TLS termination。
- Kubernetes 集成。
- 完整替代 LVS、HAProxy 或 Nginx。
- 完整用户态 TCP/IP 协议栈。
- DPDK 数据面。
- 生产级高可用集群部署。

默认不引入：

- 大型网络框架。
- 跨平台 GUI 或 Web UI。
- 数据库服务。
- 消息队列。
- DPDK。
- 与项目主线无关的代码生成框架。

数据和文档保护：

- `docs/leader/`、`docs/builder/`、`docs/reviewer/` 中的设计、实现和审查报告不得被自动覆盖。
- benchmark 报告、测试日志和阶段报告属于项目证据，应保留历史版本。
- 生成文件、构建产物和临时文件必须进入明确目录，不得混入源码和手写文档。
- 技术债和延期项的详细记录放入 `TECH-DEBT-TRACKER.md`，路线图只保留范围摘要。

## 版本路线

### V0.1 用户态 TCP 转发骨架

状态：计划中

目标：

完成最小可运行的 C++ 用户态 TCP 四层转发路径。用户可以启动负载均衡器，将本地 TCP 客户端流量经监听端口转发到一个或多个后端 echo 服务，并观察基本日志和测试结果。

核心能力：

- CMake + Ninja + LLVM 工程骨架。
- 基础 CLI 启动入口。
- 静态配置文件读取和校验。
- TCP 监听 socket。
- non-blocking I/O 与 `epoll` 事件循环。
- TCP 前后端连接绑定和双向转发。
- 基础 round-robin 后端选择。
- 最小单元测试和集成测试。

禁止范围：

- 不实现 UDP 转发。
- 不实现健康检查状态机。
- 不实现 XDP/eBPF。
- 不实现复杂多线程 reactor。
- 不引入 DPDK。
- 不实现 TLS、HTTP 路由或 L7 语义。

阶段划分：

- `S1 工程骨架与配置入口`：建立 CMake 工程、目录结构、CLI 入口、配置模型和最小测试框架。详细设计文档：`docs/leader/designs/V0.1/S1-design.md`。
- `S2 TCP 转发最小闭环`：实现 TCP 监听、后端连接、双向转发、连接关闭和基础日志。详细设计文档：`docs/leader/designs/V0.1/S2-design.md`。
- `S3 TCP 转发验证与报告`：补充本地 echo 后端验证、集成测试、最小 README 运行命令和阶段报告。详细设计文档：`docs/leader/designs/V0.1/S3-design.md`。

完成标准：

- 用户可以按 README 命令构建项目。
- 用户可以启动本地后端和负载均衡器。
- TCP 客户端流量可以通过负载均衡器转发到后端。
- 配置错误能给出明确失败信息。
- 单元测试和 TCP 集成测试通过。
- `README.md` 中的运行、测试和构建命令已同步更新。
- 相关 Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.1/`
- 实现报告：`docs/builder/reports/V0.1/`
- 审查报告：`docs/reviewer/reports/V0.1/`
- 规格文档：`docs/specs/config-schema.md`、`docs/specs/tcp-forwarding-semantics.md`

### V0.2 UDP 转发与调度策略

状态：计划中

目标：

在 TCP 基础路径之外，加入 UDP datagram 转发能力和更明确的后端调度抽象。用户可以使用同一配置模型启动 TCP 或 UDP 监听，并观察 UDP flow 与后端之间的短期绑定行为。

核心能力：

- UDP 监听和后端转发。
- UDP flow key 和 flow table。
- flow 超时清理。
- 调度策略接口。
- round-robin 策略稳定化。
- 配置中区分 TCP/UDP listener。
- UDP 单元测试和集成测试。

禁止范围：

- 不实现 XDP/eBPF。
- 不实现复杂一致性哈希。
- 不实现跨进程共享 flow table。
- 不实现完整 NAT 网关。
- 不引入数据库持久化 flow 状态。

阶段划分：

- `S1 调度抽象与配置扩展`：明确 listener、backend pool 和 scheduler 的配置关系，抽象调度接口。详细设计文档：`docs/leader/designs/V0.2/S1-design.md`。
- `S2 UDP flow table`：实现 UDP flow key、后端绑定、超时清理和响应回传。详细设计文档：`docs/leader/designs/V0.2/S2-design.md`。
- `S3 UDP 验证与语义文档`：补充 UDP 集成测试、手动验证命令和 flow table 规格文档。详细设计文档：`docs/leader/designs/V0.2/S3-design.md`。

完成标准：

- TCP 路径保持可用。
- UDP datagram 可以通过负载均衡器转发并收到后端响应。
- UDP flow 映射有明确超时行为。
- 调度策略可以被单元测试独立验证。
- 配置 schema 和 UDP 语义文档已更新。
- 相关 Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.2/`
- 实现报告：`docs/builder/reports/V0.2/`
- 审查报告：`docs/reviewer/reports/V0.2/`
- 规格文档：`docs/specs/udp-flow-table.md`、`docs/specs/scheduler.md`

### V0.3 健康检查与可观测性

状态：计划中

目标：

让负载均衡器能感知后端健康状态，并向用户暴露基础运行指标。用户可以观察后端摘除、恢复、连接数量、转发字节数和错误计数。

核心能力：

- TCP connect health check。
- 后端健康状态转换。
- 失败阈值和恢复阈值。
- 后端摘除与恢复。
- 基础 metrics snapshot。
- 日志结构化改善。
- 健康检查和指标测试。

禁止范围：

- 不实现复杂 HTTP health check。
- 不实现 Prometheus 兼容 endpoint，除非阶段设计明确纳入。
- 不实现 Web UI。
- 不实现跨进程指标聚合。
- 不实现 XDP metrics map。

阶段划分：

- `S1 健康检查状态机`：实现后端探活、状态转换、失败阈值和恢复阈值。详细设计文档：`docs/leader/designs/V0.3/S1-design.md`。
- `S2 指标模型与输出`：实现连接数、字节数、错误计数和后端状态快照。详细设计文档：`docs/leader/designs/V0.3/S2-design.md`。
- `S3 故障场景验证`：验证后端停止、恢复、连接失败和指标变化。详细设计文档：`docs/leader/designs/V0.3/S3-design.md`。

完成标准：

- 不健康后端不会继续接收新流量。
- 后端恢复后可以重新参与调度。
- 用户可以观察基础运行状态。
- 健康检查和指标模块有自动化测试。
- 故障场景有可复现验证命令或报告。
- 相关 Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.3/`
- 实现报告：`docs/builder/reports/V0.3/`
- 审查报告：`docs/reviewer/reports/V0.3/`
- 规格文档：`docs/specs/health-check.md`、`docs/specs/metrics.md`

### V0.4 用户态性能工程

状态：计划中

目标：

将用户态负载均衡器从“功能可用”推进到“可分析性能”。用户可以运行基准测试，观察不同流量场景下的吞吐、延迟、CPU 使用率和瓶颈位置。

核心能力：

- 明确 benchmark 方法。
- 本地压测脚本或命令集合。
- 连接生命周期优化。
- 缓冲区管理优化。
- 资源释放和 graceful shutdown。
- 性能报告。
- 回归测试保护已有 TCP/UDP 行为。

禁止范围：

- 不提前实现 XDP/eBPF。
- 不引入 DPDK。
- 不把 benchmark 结果写成不可复现的宣传数字。
- 不引入复杂多线程模型，除非阶段设计证明必要。

阶段划分：

- `S1 Benchmark 方法与工具`：定义测试环境、工具、指标和报告格式。详细设计文档：`docs/leader/designs/V0.4/S1-design.md`。
- `S2 用户态资源与生命周期优化`：改善缓冲、连接关闭、错误路径和 graceful shutdown。详细设计文档：`docs/leader/designs/V0.4/S2-design.md`。
- `S3 性能报告与回归验证`：生成可复现性能报告，保护已有行为不回退。详细设计文档：`docs/leader/designs/V0.4/S3-design.md`。

完成标准：

- benchmark 方法文档化。
- 用户可以复现至少一组 TCP 和 UDP 性能测试。
- 已知瓶颈有记录。
- 行为回归测试通过。
- README、架构文档和技术债记录已同步必要变化。
- 相关 Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V0.4/`
- 实现报告：`docs/builder/reports/V0.4/`
- 审查报告：`docs/reviewer/reports/V0.4/`
- 性能文档：`docs/benchmarks/methodology.md`、`docs/benchmarks/reports/`

### V1.0 用户态稳定版

状态：计划中

目标：

发布一个稳定的用户态 L4 负载均衡器版本。用户可以通过清晰配置运行 TCP/UDP 负载均衡，使用测试和验证命令确认行为，并通过文档理解架构边界和已知限制。

核心能力：

- TCP/UDP 用户态转发稳定。
- 配置 schema 稳定。
- 基础调度、健康检查和指标稳定。
- 构建、运行、测试文档完整。
- 关键行为有自动化测试保护。
- 已知限制和技术债明确记录。

禁止范围：

- 不包含 XDP/eBPF 数据面。
- 不包含 DPDK。
- 不承诺生产级替代 LVS、HAProxy 或 Nginx。
- 不承诺跨平台运行。

阶段划分：

- `S1 用户态行为冻结`：冻结 V1.0 用户可见语义，清理配置、日志和错误输出。详细设计文档：`docs/leader/designs/V1.0/S1-design.md`。
- `S2 文档与验收补齐`：补齐 README、架构、规格、测试和 benchmark 文档。详细设计文档：`docs/leader/designs/V1.0/S2-design.md`。
- `S3 发布审查`：完成 Reviewer 审查、技术债归档和发布前验证。详细设计文档：`docs/leader/designs/V1.0/S3-design.md`。

完成标准：

- README 中最短运行路径可用。
- 用户态 TCP/UDP 主流程稳定通过测试。
- 配置、TCP 语义、UDP flow、调度、健康检查和指标文档齐备。
- 关键限制记录到 `TECH-DEBT-TRACKER.md`。
- `CHANGELOG.md` 记录 V1.0 变化。
- 相关 Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V1.0/`
- 实现报告：`docs/builder/reports/V1.0/`
- 审查报告：`docs/reviewer/reports/V1.0/`
- 规格文档：`docs/specs/`

### V1.1 XDP/eBPF 基础集成

状态：计划中

目标：

在不破坏用户态稳定路径的前提下，引入 XDP/eBPF 基础构建和加载能力。用户可以编译 eBPF object，在合适 Linux 环境中加载最小 XDP 程序，并观察基础 attach/detach 行为。

核心能力：

- eBPF 程序目录和构建规则。
- 最小 XDP 程序。
- 用户态 loader 或 libbpf 集成。
- attach/detach 流程。
- XDP 环境检测和错误提示。
- XDP 测试分层标记。

禁止范围：

- 不要求 WSL2 完成 native XDP 性能验证。
- 不把 XDP 作为默认运行路径。
- 不实现完整 XDP 负载均衡。
- 不替换用户态 TCP/UDP 转发。
- 不引入 DPDK。

阶段划分：

- `S1 eBPF 构建骨架`：建立 BPF 目标、目录结构、最小 XDP 程序和构建命令。详细设计文档：`docs/leader/designs/V1.1/S1-design.md`。
- `S2 用户态加载与环境检查`：实现 attach/detach、设备参数、权限错误和环境提示。详细设计文档：`docs/leader/designs/V1.1/S2-design.md`。
- `S3 XDP 文档与验证`：记录 Linux 环境要求、WSL2 限制和最小验证流程。详细设计文档：`docs/leader/designs/V1.1/S3-design.md`。

完成标准：

- 用户态路径仍可独立运行和测试。
- eBPF object 可以通过构建命令生成。
- 在合适 Linux 环境中可以执行最小 attach/detach 验证。
- XDP 相关失败有明确错误提示。
- XDP 环境文档和 map schema 初稿已生成。
- 相关 Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V1.1/`
- 实现报告：`docs/builder/reports/V1.1/`
- 审查报告：`docs/reviewer/reports/V1.1/`
- XDP 文档：`docs/specs/xdp-map-schema.md`、`docs/runbooks/linux-xdp-env.md`

### V1.2 XDP L4 Fast Path 原型

状态：计划中

目标：

实现一个受限范围内的 XDP L4 fast path 原型，用于验证包级解析、map 查询、统计更新和可回退的 fast path 设计。该版本强调理解边界和性能验证，不承诺生产级完整 TCP 代理语义。

核心能力：

- XDP 包头解析。
- 后端信息 eBPF map。
- 基础统计 map。
- UDP 或受限 L4 转发原型。
- 用户态控制面更新 eBPF maps。
- fast path 与用户态路径的边界说明。
- 初步性能对比报告。

禁止范围：

- 不实现完整 TCP proxy。
- 不实现复杂 conntrack。
- 不实现生产级 NAT/LB。
- 不把 XDP 性能结果泛化到 WSL2。
- 不引入 DPDK。

阶段划分：

- `S1 map schema 与控制面同步`：定义后端、统计和必要会话 map 的 key/value 结构。详细设计文档：`docs/leader/designs/V1.2/S1-design.md`。
- `S2 XDP 包处理原型`：实现包头解析、map 查询、统计更新和受限转发行为。详细设计文档：`docs/leader/designs/V1.2/S2-design.md`。
- `S3 性能对比与边界总结`：对比用户态路径和 XDP fast path，记录适用场景与限制。详细设计文档：`docs/leader/designs/V1.2/S3-design.md`。

完成标准：

- XDP 原型可以在合适 Linux 环境中运行。
- 用户态控制面可以更新 XDP 所需 maps。
- XDP 统计可以被读取或展示。
- 用户态路径不被破坏。
- 性能对比方法和结果可复现。
- XDP 限制和后续技术债已记录。
- 相关 Builder 报告和 Reviewer 审查报告已生成。

相关文档：

- 设计文档：`docs/leader/designs/V1.2/`
- 实现报告：`docs/builder/reports/V1.2/`
- 审查报告：`docs/reviewer/reports/V1.2/`
- 规格文档：`docs/specs/xdp-map-schema.md`
- 性能文档：`docs/benchmarks/reports/`

## 阶段设计摘要

阶段设计摘要只固定版本内部的大块交付，不展开到任务级别。每个阶段开始前，Leader 应在对应路径创建详细设计文档。

| 版本 | 阶段 | 阶段目标 | 主要产出 | 涉及区域 | 详细设计 |
| --- | --- | --- | --- | --- | --- |
| `V0.1` | `S1 工程骨架与配置入口` | 建立最小 C++ 工程和配置入口 | CMake 骨架、CLI、配置模型、测试框架 | `src/cli/`、`src/config/`、`tests/` | `docs/leader/designs/V0.1/S1-design.md` |
| `V0.1` | `S2 TCP 转发最小闭环` | 打通 TCP 监听到后端转发 | TCP 监听、后端连接、双向转发 | `src/net/`、`src/core/`、`src/control/` | `docs/leader/designs/V0.1/S2-design.md` |
| `V0.1` | `S3 TCP 转发验证与报告` | 让 TCP 路径可验证、可报告 | 集成测试、运行命令、阶段报告 | `tests/`、`docs/builder/`、`docs/reviewer/` | `docs/leader/designs/V0.1/S3-design.md` |
| `V0.2` | `S1 调度抽象与配置扩展` | 稳定调度和 listener 配置模型 | scheduler 接口、配置扩展 | `src/core/`、`src/config/` | `docs/leader/designs/V0.2/S1-design.md` |
| `V0.2` | `S2 UDP flow table` | 实现 UDP 转发和 flow 映射 | flow key、flow table、超时清理 | `src/net/`、`src/core/` | `docs/leader/designs/V0.2/S2-design.md` |
| `V0.2` | `S3 UDP 验证与语义文档` | 验证 UDP 行为并文档化 | UDP 测试、flow table spec | `tests/`、`docs/specs/` | `docs/leader/designs/V0.2/S3-design.md` |
| `V0.3` | `S1 健康检查状态机` | 建立后端健康状态机制 | health checker、状态转换测试 | `src/health/`、`src/control/` | `docs/leader/designs/V0.3/S1-design.md` |
| `V0.3` | `S2 指标模型与输出` | 暴露基础运行状态 | metrics snapshot、日志改善 | `src/metrics/`、`src/control/` | `docs/leader/designs/V0.3/S2-design.md` |
| `V0.3` | `S3 故障场景验证` | 验证后端故障和恢复 | 故障测试、验证报告 | `tests/`、`docs/reviewer/` | `docs/leader/designs/V0.3/S3-design.md` |
| `V0.4` | `S1 Benchmark 方法与工具` | 建立性能验证方法 | benchmark methodology、工具命令 | `docs/benchmarks/`、`tests/` | `docs/leader/designs/V0.4/S1-design.md` |
| `V0.4` | `S2 用户态资源与生命周期优化` | 改善连接和资源管理 | shutdown、缓冲、错误路径优化 | `src/net/`、`src/control/` | `docs/leader/designs/V0.4/S2-design.md` |
| `V0.4` | `S3 性能报告与回归验证` | 生成可复现性能证据 | benchmark report、回归测试 | `docs/benchmarks/`、`tests/` | `docs/leader/designs/V0.4/S3-design.md` |
| `V1.0` | `S1 用户态行为冻结` | 稳定用户态语义 | 配置、日志、错误输出冻结 | 全部用户态模块 | `docs/leader/designs/V1.0/S1-design.md` |
| `V1.0` | `S2 文档与验收补齐` | 补齐稳定版文档和验收 | specs、README、测试说明 | `docs/specs/`、根文档 | `docs/leader/designs/V1.0/S2-design.md` |
| `V1.0` | `S3 发布审查` | 完成稳定版审查 | review report、changelog、技术债归档 | `docs/reviewer/`、`CHANGELOG.md` | `docs/leader/designs/V1.0/S3-design.md` |
| `V1.1` | `S1 eBPF 构建骨架` | 建立 XDP 编译基础 | BPF object、最小 XDP 程序 | `src/xdp/`、构建系统 | `docs/leader/designs/V1.1/S1-design.md` |
| `V1.1` | `S2 用户态加载与环境检查` | 支持 XDP attach/detach | loader、环境错误提示 | `src/xdp/`、`src/control/` | `docs/leader/designs/V1.1/S2-design.md` |
| `V1.1` | `S3 XDP 文档与验证` | 文档化 XDP 环境和验证 | runbook、最小验证报告 | `docs/runbooks/`、`docs/reviewer/` | `docs/leader/designs/V1.1/S3-design.md` |
| `V1.2` | `S1 map schema 与控制面同步` | 定义 eBPF map 边界 | xdp-map-schema、同步逻辑 | `src/xdp/`、`src/control/` | `docs/leader/designs/V1.2/S1-design.md` |
| `V1.2` | `S2 XDP 包处理原型` | 实现受限 fast path | 包头解析、map 查询、统计更新 | `src/xdp/` | `docs/leader/designs/V1.2/S2-design.md` |
| `V1.2` | `S3 性能对比与边界总结` | 评估 XDP 原型价值 | 对比报告、限制总结 | `docs/benchmarks/`、`TECH-DEBT-TRACKER.md` | `docs/leader/designs/V1.2/S3-design.md` |

## 长期演进方向

远期方向保持概括，不作为当前版本承诺：

- 更多调度策略，例如 weighted round-robin、least-connections 或一致性哈希。
- 更完整的 metrics 输出方式，例如文本 endpoint 或 Prometheus 兼容格式。
- 配置热加载和连接 draining。
- 更系统的故障注入测试。
- 多线程 reactor 或 per-core 数据面设计。
- 更深入的 XDP map 设计和 fast path 适用范围评估。
- 与独立 DPDK L3 forwarding 或 NAT 项目进行方法论对照，但不在本仓库实现 DPDK 数据面。

进入 XDP/eBPF 相关版本前必须满足：

- 用户态 TCP/UDP 路径稳定。
- 配置、调度、健康检查和指标语义清晰。
- 默认测试可以稳定通过。
- 性能测试方法已文档化。
- 已明确可用的 Linux XDP 验证环境。

## 变更记录

- `2026-05-19`：创建初版路线图，确定项目从 C++ 用户态 TCP/UDP L4 负载均衡器演进到 XDP/eBPF fast path 的版本边界。
