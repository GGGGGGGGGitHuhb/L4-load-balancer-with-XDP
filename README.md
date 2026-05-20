# L4 Load Balancer with XDP

本项目是一个面向高性能网络方向的四层负载均衡器实验项目。项目先使用 C++ 在用户态实现可测试、可观测的 TCP/UDP L4 负载均衡器，再逐步引入 XDP/eBPF fast path，探索 Linux 网络栈中的高性能包处理与内核态数据面设计。

项目目标不是只完成一个能转发流量的 demo，而是围绕四层负载均衡的核心问题建立一套可演进的工程实现：连接与会话管理、后端调度、健康检查、可观测性、性能测试，以及用户态控制面与 XDP 数据面的协作。

## 当前状态

- 当前版本：未发布
- 状态：项目初始化，文档与工程骨架建设中
- 主要能力：
  - 明确项目方向：C++ 用户态 L4 负载均衡器 + XDP/eBPF fast path
  - 明确基础技术栈：LLVM、CMake、Ninja、C++20、Linux socket、epoll
  - 明确演进目标：先完成用户态 TCP/UDP 负载均衡，再实现 XDP/eBPF 数据面原型

## 功能概览

当前仓库尚未包含可运行实现。第一阶段实现完成后，项目将优先提供以下用户态能力：

- TCP 四层转发与后端连接管理。
- UDP 会话转发与短期 flow table。
- Round-robin 等基础后端调度策略。
- 后端健康检查与摘除恢复。
- 运行状态、连接数、转发字节数等基础指标。

XDP/eBPF 相关能力将在用户态版本稳定后引入，详细阶段范围以 `ROADMAP.md` 为准。

## 环境要求

当前目标开发环境：

- 操作系统：Linux 或 WSL2
- 编译器：Clang/LLVM
- 构建系统：CMake + Ninja
- 语言标准：C++20
- 网络能力：Linux socket、epoll
- 后续 XDP/eBPF 依赖：Linux 内核、Clang BPF target、libbpf 或等价加载方式

说明：

- WSL2 适合开发用户态负载均衡器、运行基础测试和编译验证。
- XDP/eBPF 的真实挂载与性能验证建议在 Linux 裸机、双系统或具备合适内核与网卡能力的 Linux 环境中完成。
- 当前仓库尚未引入外部数据库或独立外部服务。

## 快速开始

当前项目仍处于初始化阶段，尚未提供可运行程序。后续工程骨架建立后，本节会同步更新为最短可运行路径。

运行目录：

    <repo-root>

当前可执行操作：

    git status

预期看到：

    在 WSL shell 或仓库根目录执行时，显示当前仓库状态，以及尚未实现业务代码的项目骨架。

## 常用命令

当前尚未提供稳定的构建、运行和测试命令。项目引入 CMake 工程后，本节会保留确实存在且推荐使用的命令，例如：

构建项目：

    cmake -S . -B build -G Ninja
    cmake --build build

运行测试：

    ctest --test-dir build

以上命令为目标工程形态，需等待 CMake 工程文件与测试目标建立后使用。

## 配置说明

当前尚未定义正式配置文件。第一阶段预计使用静态配置文件描述监听地址、后端节点、调度策略和健康检查参数。

- 配置文件：待定
- 默认配置：待定
- 示例配置：待定
- 如何覆盖默认值：待定

配置格式和字段含义将在实现落地后补充，并在架构文档中说明关键设计取舍。

## 项目结构

当前仓库结构：

    .
    ├── README.md
    ├── ROADMAP.md
    ├── ARCHITECTURE.md
    ├── CHANGELOG.md
    ├── TECH-DEBT-TRACKER.md
    ├── AGENTS.md
    ├── .clang-format
    └── docs/
        ├── leader/
        ├── builder/
        ├── reviewer/
        └── references/

说明：

- `README.md`：项目入口，说明项目定位、当前状态、环境要求和重要文档。
- `ROADMAP.md`：版本路线和阶段范围。
- `ARCHITECTURE.md`：系统结构、模块职责和数据流。
- `CHANGELOG.md`：记录已完成版本变化。
- `TECH-DEBT-TRACKER.md`：记录跨版本技术债、已知风险和延后事项。
- `AGENTS.md`：记录项目协作规则和开发约束。
- `.clang-format`：记录 C++ 代码格式化规则，当前基于 Google 风格。
- `docs/leader/`：阶段目标、设计说明和决策记录。
- `docs/builder/`：实现计划与实现报告。
- `docs/reviewer/`：审查清单、审查报告和问题记录。
- `docs/references/`：阶段设计、审查设计等文档模板或撰写规范。

## 测试与验证

当前尚未提供自动化测试。后续至少会包含：

- 自动化单元测试：验证调度策略、配置解析、会话表、健康检查等模块。
- 集成测试：验证 TCP/UDP 转发、后端故障摘除和恢复。
- 手动验证：使用本地 echo server、`nc`、`curl`、`iperf` 或自定义压测工具验证转发行为。
- 性能验证：记录用户态版本与 XDP/eBPF fast path 的吞吐、延迟和 CPU 使用情况。

测试命令将在测试框架建立后补充到本节。

## 文档索引

- `ROADMAP.md`：版本路线和阶段范围。
- `ARCHITECTURE.md`：系统结构、模块职责和数据流。
- `CHANGELOG.md`：已完成版本变化。
- `TECH-DEBT-TRACKER.md`：跨版本技术债和风险。
- `AGENTS.md`：项目协作规则、代码约定和测试要求。
- `docs/leader/`：阶段详细设计、技术取舍和决策记录。
- `docs/builder/`：实现报告和构建过程记录。
- `docs/reviewer/`：审查报告、风险评估和验收记录。

## 开发流程

当前项目采用文档先行、阶段推进的开发方式：

- 先在 `README.md`、`ARCHITECTURE.md` 和 `ROADMAP.md` 中明确项目范围、系统设计和版本路线。
- 每个阶段开始前，由 `docs/leader/` 记录目标、边界和设计取舍。
- 实现过程中，由 `docs/builder/` 记录任务拆解、实现结果和验证方式。
- 阶段完成后，由 `docs/reviewer/` 记录审查结果、风险和遗留问题。
- 重要行为变化更新 `CHANGELOG.md`。
- 跨阶段技术债和延后事项记录到 `TECH-DEBT-TRACKER.md`。

## 已知限制

- 当前仓库尚未包含可运行代码。
- 当前尚未确定配置文件格式和目录结构。
- 当前尚未提供自动化测试、集成测试和性能测试。
- WSL2 不适合作为 XDP/eBPF native mode 的最终性能验证环境。
- 本项目主线聚焦 L4 负载均衡与 XDP/eBPF，不包含 DPDK 实现。

详细长期技术债和风险将记录到 `TECH-DEBT-TRACKER.md`。

## 许可证

暂未指定。
