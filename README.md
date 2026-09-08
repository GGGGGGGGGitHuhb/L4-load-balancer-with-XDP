# L4 Load Balancer with XDP

本项目是一个面向高性能网络方向的四层负载均衡器实验项目。项目先使用 C++ 在用户态实现可测试、可观测的 TCP/UDP L4 负载均衡器，再逐步引入 XDP/eBPF fast path，探索 Linux 网络栈中的高性能包处理与内核态数据面设计。

项目目标不是只完成一个能转发流量的 demo，而是围绕四层负载均衡的核心问题建立一套可演进的工程实现：连接与会话管理、后端调度、健康检查、可观测性、性能测试，以及用户态控制面与 XDP 数据面的协作。

本项目定位为传输层网络基础设施项目，重点区别于应用层 HTTP 服务器：HTTP 服务器关注请求解析、响应生成和应用层并发处理，本项目关注 TCP/UDP 四层转发、后端调度、UDP flow table、健康检查、控制面与数据面分离，以及后续 XDP/eBPF 包级 fast path 验证。

## 当前状态

- 当前版本：未发布
- 状态：V0.1/S1 已完成；S2 TCP 转发已完成（Completed，独立复审 PASS）；S3 已完成（Completed，独立审查 PASS），V0.1 开发范围完成；本地 main 已包含 S3 合并提交 544c8d8，远端发布状态本轮未核验
- 主要能力：
  - 明确项目方向：C++ 用户态 L4 负载均衡器 + XDP/eBPF fast path
  - 明确基础技术栈：LLVM、CMake、Ninja、C++20、Linux socket、epoll
  - 明确演进目标：先完成用户态 TCP/UDP 负载均衡，再实现 XDP/eBPF 数据面原型

## 功能概览

当前提供离线 C++20 构建、静态 TCP/UDP 配置检查、固定轮询 TCP 双向转发、有界背压及半关闭。

UDP 数据面、健康检查、指标和 XDP/eBPF 由后续阶段引入，范围见 `ROADMAP.md`。

## 环境要求

当前目标开发环境：

- 操作系统：Linux 或 WSL2
- 编译器：Clang++/LLVM
- 构建系统：CMake + Ninja
- 语言标准：C++20
- 网络能力：Linux socket、epoll
- 后续 XDP/eBPF 依赖：Linux 内核、Clang BPF target、libbpf 或等价加载方式

说明：

- WSL2 适合开发用户态负载均衡器、运行基础测试和编译验证。
- XDP/eBPF 的功能验证和阶段收尾优先使用云服务器 Linux 环境完成。
- 云服务器上的 XDP/eBPF 验证用于证明编译、加载、attach/detach、map 交互和包级处理路径；普通云服务器性能结果只代表对应云环境，不泛化为物理网卡 native XDP 极限性能。
- 当前仓库尚未引入外部数据库或独立外部服务。

## 快速开始

在 Linux/WSL2 仓库根目录运行，要求 CMake >= 3.20、Ninja 和支持 C++20 的 Clang++。不需要网络、第三方下载、root 或 XDP 环境。

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/l4lb --help
./build/bin/l4lb --check-config configs/example.conf
```

配置检查预期输出：`配置有效：TCP，后端数量=2`。这只表示配置有效，不表示后端可达。

## TCP 最短运行示例

默认开启测试时构建的 `tcp_echo_backend` 是前台演示 fixture。依次在三个终端运行：

```bash
# 终端 1
./build/bin/tcp_echo_backend 9001
# 终端 2
./build/bin/tcp_echo_backend 9002
# 终端 3：见到“TCP 服务已启动”再连接
./build/bin/l4lb --run configs/example.conf
```

第四个 Bash 终端发送并读取相同字节（无需安装 nc）：

```bash
exec 3<>/dev/tcp/127.0.0.1/8080
printf 'hello S2\n' >&3
IFS= read -r reply <&3
printf '%s\n' "$reply"
exec 3<&- 3>&-
```

预期输出 `hello S2`。结束时在三个服务终端分别 Ctrl+C。代理的 SIGINT/SIGTERM 会立即关闭会话，**不保证在途数据排空**。示例后端是单连接顺序 echo fixture，不是产品依赖。详细限制见 [TCP 转发语义](docs/specs/tcp-forwarding-semantics.md)。

## 自动产品验收（S3）

默认测试构建完成后，可直接验证真实产品进程：

```bash
./build/bin/tcp_product_smoke ./build/bin/l4lb ./build/bin/tcp_echo_backend ./build/product-evidence
ctest --test-dir build -L s3 --output-on-failure
```

正常退出 0 并输出 PASS 与本轮证据目录；失败退出 1，用法错误退出 2。新增入口覆盖二进制轮询、1 MiB + 17 字节半关闭、后端停机后恢复及启动失败，所有进程均有界清理。全套现有 6 项；echo 和验收程序仅在 BUILD_TESTING=ON 时生成。

重复/并行运行、失败复现、生产独立构建和环境边界见[本地 TCP 验证手册](docs/runbooks/local-tcp-validation.md)，版本证据见[V0.1 验收矩阵](docs/specs/v0.1-acceptance.md)。S3 独立审查 PASS：Debug/Release 各 6/6、各连续 3 次产品测试和双实例并行通过，三类强制负向共 6 次准确退出 1 并清理。Leader 已收尾，V0.1 开发范围完成，尚未发布。

## 常用命令

Release 构建与测试：

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

程序统一位于各构建目录的 `bin/`；CLI、S2 和 S3 测试临时文件分别位于其 `test-tmp/`、`test-s2/` 和 `product-evidence/`。测试默认开启，可通过 `-DBUILD_TESTING=OFF` 关闭。

## 配置说明

使用限定的 `key=value` 行格式：必填一个 `listen` 和 1 至 256 个有序 `backend`，均为数字 IPv4:端口，没有默认端点。

- [示例配置](configs/example.conf)。
- 可选 `protocol=tcp|udp`（默认 `tcp`）、`scheduler=round_robin`（默认同值），每项至多一次，键值区分大小写；UDP 仅允许 `--check-config`，`--run` 在创建网络资源前退出 1。
- [调度规格](docs/specs/scheduler.md)：独立轮询状态、失败仍推进一次且不重试。
- [配置规格](docs/specs/config-schema.md)：严格数字、空白、注释、大小限制与错误规则。
- 配置路径相对当前工作目录，空格路径需要 shell 引号；不搜索默认配置。
- 成功退出 0；文件或配置错误退出 1；CLI 用法错误退出 2。失败信息写入 stderr。
- 输入只读，普通文件符号链接允许，目录/FIFO/设备拒绝；读取上限为 64 KiB。

## 项目结构

当前仓库结构：

    .
    ├── CMakeLists.txt
    ├── src/cli/
    ├── src/config/
    ├── src/core/
    ├── src/net/
    ├── src/control/
    ├── tests/
    ├── configs/example.conf
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

CTest 包含配置单元测试和 CLI 进程集成测试，覆盖合法输入、拒绝规则、端点顺序、64 KiB 边界、精确退出码与输出、FIFO 超时、普通文件符号链接及配置内容不变。

普通用户还验证不可读文件；root 环境会明确输出该权限用例未验证。Debug/Release 使用显式检查，不依赖 `assert`。S2 增加真实 TCP 进程集成与定向状态测试，覆盖双向二进制、ABCABC/并发、半关闭/RST、超时、慢读背压、容量、fd/token 回收及 HUP 退避。性能验证属于后续阶段。

S2 可单独运行：

```bash
ctest --test-dir build -L s2 --output-on-failure
```

完整测试包含 `config_unit`、`cli_integration`、`s2_net_unit`、`s2_tcp_integration`、`s2_reactor_state`。各测试有总超时，无外网依赖。进程集成的配置、stdout/stderr 和分支 trace 保存到构建目录 `test-s2/`；测试只管理自己创建的子进程和 fd。

## 文档索引

- `ROADMAP.md`：版本路线和阶段范围。
- `ARCHITECTURE.md`：系统结构、模块职责和数据流。
- `CHANGELOG.md`：已完成版本变化。
- `TECH-DEBT-TRACKER.md`：跨版本技术债和风险。
- `AGENTS.md`：项目协作规则、代码约定和测试要求。
- `docs/leader/`：阶段详细设计、技术取舍和决策记录。
- `docs/builder/`：实现报告和构建过程记录。
- `docs/reviewer/`：审查报告、风险评估和验收记录。

## 当前阶段入口

V0.2/S1 已批准（分支 `v0.2-s1`，V0.2-S1-D1 Approved），当前 Completed，独立 Reviewer002 复审 PASS（Debug/Release 各 7/7），F-001 已关闭，Leader 已完成最终状态同步。S1 聚焦调度抽象和配置扩展，UDP 转发留在 S2。

- [V0.2/S1 已批准设计](docs/leader/designs/V0.2/S1-design.md)
- [V0.2/S1 审查计划](docs/reviewer/reviews/V0.2/S1-review.md)
- [V0.2/S1 准备报告](docs/leader/reports/V0.2/S1-report-001.md)
- [V0.2/S1 批准登记](docs/leader/reports/V0.2/S1-report-002.md)
- [V0.2/S1 首轮审查](docs/reviewer/reports/V0.2/S1-report-001.md)：保留首轮 FAIL，F-001 已关闭。
- [V0.2/S1 复审报告](docs/reviewer/reports/V0.2/S1-report-002.md)：PASS，F-001 Closed。
- [V0.2/S1 完成报告](docs/leader/reports/V0.2/S1-report-003.md)：Completed；S2/S3 未开始。

以下保留 V0.1 阶段交付入口。

S3-D1 保持 Approved；S3 已完成，独立 Reviewer PASS，Leader 已核对 V0.1 七条完成标准。V0.1 开发范围完成；本地 main 已包含 S3 合并提交 544c8d8，远端发布状态本轮未核验。

- [S3 完成报告](docs/leader/reports/V0.1/S3-report-003.md)：批准、交付、独立验收与版本收尾。
- [S3 审查报告](docs/reviewer/reports/V0.1/S3-report-001.md)：AC-01..07 PASS 与验证边界。

- [S3 详细设计](docs/leader/designs/V0.1/S3-design.md)：真实产品验证与 V0.1 验收范围。
- [S3 审查计划](docs/reviewer/reviews/V0.1/S3-review.md)：重复运行、失败判定与独立验收。
- [S3 准备报告](docs/leader/reports/V0.1/S3-report-001.md)：前置检查和决策摘要。

S1/S2 已完成；S2-D1 保持 Approved，Reviewer002 复审 PASS，S2-R001 已关闭；S3 已独立审查 PASS 并完成收尾，V0.1 开发范围完成。

- [S2 完成报告](docs/leader/reports/V0.1/S2-report-004.md)：交付、验收与返工闭环。

- [S2 返工报告](docs/builder/reports/V0.1/S2-report-002.md)：S2-R001 后端断言传播修复及正负验证。
- [S2 实现报告](docs/builder/reports/V0.1/S2-report-001.md)：逐项自测、原始证据与限制。
- [S2 复审报告](docs/reviewer/reports/V0.1/S2-report-002.md)：PASS，八次负向验证准确失败，S2-R001 已关闭。
- [S2 首轮审查](docs/reviewer/reports/V0.1/S2-report-001.md)：保留首轮 FAIL 与问题复现历史。
- [S2 详细设计](docs/leader/designs/V0.1/S2-design.md)：转发、背压、关闭与验收契约。
- [S2 审查计划](docs/reviewer/reviews/V0.1/S2-review.md)：错误路径和动态证据要求。
- [S2 准备报告](docs/leader/reports/V0.1/S2-report-001.md)：范围、限制与下一步。

- [S1 详细设计](docs/leader/designs/V0.1/S1-design.md)：范围、CLI、配置规则与验收标准。
- [S1 完成报告](docs/leader/reports/V0.1/S1-report-003.md)：批准、实现、独立验收与收尾证据。
- [S1 审查报告](docs/reviewer/reports/V0.1/S1-report-001.md)：独立验收 PASS，验证证据与限制。
- [S1 审查计划](docs/reviewer/reviews/V0.1/S1-review.md)：实现后的复核步骤。
- [启动准备报告](docs/leader/reports/V0.1/S1-report-001.md)：准备结果与剩余阶段交付。
- [本机环境验证](docs/runbooks/local-dev-env.md)：工具链与本地 TCP 验证证据。

角色指南与设计目录按当前 `.gitignore` 约定仅在本地保留；新克隆不会自带这些协作材料，跨机器交接需另行同步。

## 开发流程

当前项目采用文档先行、阶段推进的开发方式：

- 先在 `README.md`、`ARCHITECTURE.md` 和 `ROADMAP.md` 中明确项目范围、系统设计和版本路线。
- 每个阶段开始前，由 `docs/leader/` 记录目标、边界和设计取舍。
- 实现过程中，由 `docs/builder/` 记录任务拆解、实现结果和验证方式。
- 阶段完成后，由 `docs/reviewer/` 记录审查结果、风险和遗留问题。
- 重要行为变化更新 `CHANGELOG.md`。
- 跨阶段技术债和延后事项记录到 `TECH-DEBT-TRACKER.md`。

## 已知限制

- TCP 代理采用单线程与固定资源上限，停止时不等待在途字节排空。
- 仅支持静态数字 IPv4 TCP 端点，不支持 DNS、IPv6 或热加载。
- 当前只提供 TCP 静态轮询转发，不提供 UDP、健康检查、失败切换或性能承诺。
- WSL2 不适合作为 XDP/eBPF native mode 的最终性能验证环境。
- XDP/eBPF 阶段计划使用云服务器进行功能验证和收尾；性能结论必须标注云环境限制。
- 本项目主线聚焦 L4 负载均衡与 XDP/eBPF，不包含 DPDK 实现。

详细长期技术债和风险将记录到 `TECH-DEBT-TRACKER.md`。

## 许可证

暂未指定。
