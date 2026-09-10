# L4 Load Balancer with XDP

本项目是一个面向高性能网络方向的四层负载均衡器实验项目。项目先使用 C++ 在用户态实现可测试、可观测的 TCP/UDP L4 负载均衡器，再逐步引入 XDP/eBPF fast path，探索 Linux 网络栈中的高性能包处理与内核态数据面设计。

项目目标不是只完成一个能转发流量的 demo，而是围绕四层负载均衡的核心问题建立一套可演进的工程实现：连接与会话管理、后端调度、健康检查、可观测性、性能测试，以及用户态控制面与 XDP 数据面的协作。

本项目定位为传输层网络基础设施项目，重点区别于应用层 HTTP 服务器：HTTP 服务器关注请求解析、响应生成和应用层并发处理，本项目关注 TCP/UDP 四层转发、后端调度、UDP flow table、健康检查、控制面与数据面分离，以及后续 XDP/eBPF 包级 fast path 验证。

## 当前状态

- 最近已核验阶段标签：`v0.4-s2`，合并提交 `48a1283`（2026-09-10本地/远端注释对象及peeled核验一致）。
- V0.1、V0.2、V0.3开发范围均Completed；V0.4/S1、S2均已完成并合并/标记，S2 Builder001、Reviewer001 PASS、Leader003齐备。
- 当前分支 `codex/v0.4-s3`，基于最新main / `v0.4-s2`；S3性能报告与回归验证已通过 [Reviewer001独立验收](docs/reviewer/reports/V0.4/S3-report-001.md)：最终Debug28/28、Release29/29、独立48run及公开原始包数学/全表重算通过。V0.4-S3-D1 Approved，S3及V0.4开发范围Completed，Leader003收尾完成；尚未发布V1.0。
- S3准备：[阶段设计](docs/leader/designs/V0.4/S3-design.md)、[审查方案](docs/reviewer/reviews/V0.4/S3-review.md)、[Leader准备报告](docs/leader/reports/V0.4/S3-report-001.md)、[批准登记](docs/leader/reports/V0.4/S3-report-002.md)、[完成报告](docs/leader/reports/V0.4/S3-report-003.md)。
- S2记录：[批准设计](docs/leader/designs/V0.4/S2-design.md)、[独立验收PASS](docs/reviewer/reports/V0.4/S2-report-001.md)、[完成报告](docs/leader/reports/V0.4/S2-report-003.md)。
- 主要能力：
  - 明确项目方向：C++ 用户态 L4 负载均衡器 + XDP/eBPF fast path
  - 明确基础技术栈：LLVM、CMake、Ninja、C++20、Linux socket、epoll
  - 明确演进目标：先完成用户态 TCP/UDP 负载均衡，再实现 XDP/eBPF 数据面原型

## 功能概览

当前提供离线 C++20 构建、静态 TCP/UDP 配置检查、固定轮询 TCP 双向转发、有界背压及半关闭。

当前增加 UDP 按 flow 双向数据报转发；增加默认关闭的 TCP 握手健康检查；已增加默认关闭的指标stderr快照；XDP/eBPF 由后续阶段引入，范围见 `ROADMAP.md`。

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
- 可选 `protocol=tcp|udp`（默认 `tcp`）、`scheduler=round_robin`（默认同值），每项至多一次，键值区分大小写；UDP 支持 `--check-config` 和 `--run`，按 flow 固定后端、空闲 60 秒、容量 1024、尽力丢弃且不重试。
- [完整UDP验证手册](docs/runbooks/local-udp-validation.md)：普通clone手动工具、wildcard/后端恢复、固定生产60秒长项与证据边界。
- [V0.2完成矩阵](docs/specs/v0.2-acceptance.md)：六条标准、继承/新增和未验证范围。
- [UDP flow 规格](docs/specs/udp-flow-table.md)：地址关联、报文边界、过期/容量、错误和限制。
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

当前注册29项；Debug快速执行 `ctest --test-dir build -LE udp_long`（28项），Release完整29项含一次约61秒原生产过期测试。原26项保留（含统计及TCP/UDP短benchmark工具smoke），V0.4/S2新增ring/生命周期与真实停机2项（`v04_lifecycle`）；S3新增1项比较纯校验（`v04_compare`）；长性能矩阵不加入CTest。`udp_fast`两项可重复/并行，`udp_long`不加入快速组；容量1024只作静态默认值确认，未做1024满载产品验收。

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

当前V0.3/S3及V0.3开发范围Completed，分支 `codex/v0.3-s3`，V0.3-S3-D1保持Approved；Builder001、Reviewer001独立PASS及Leader003收尾齐备，六条版本标准全部满足。范围为双后端故障/恢复、业务连接拒绝、指标联动及公开V0.3验收手册，不新增产品功能或性能承诺。

- [V0.3/S3批准设计](docs/leader/designs/V0.3/S3-design.md)
- [V0.3/S3审查计划](docs/reviewer/reviews/V0.3/S3-review.md)
- [V0.3/S3准备决策](docs/leader/reports/V0.3/S3-report-001.md)
- [V0.3/S3批准登记](docs/leader/reports/V0.3/S3-report-002.md)
- [V0.3/S3与版本完成报告](docs/leader/reports/V0.3/S3-report-003.md)

### 已完成的V0.3/S2

S2 Completed，Builder002、Reviewer002 PASS、Leader003齐备；已合并PR8并标记v0.3-s2（2f5c26d，含dfcffe6）。当前S3与V0.3完成状态见上节。

- [V0.3/S2批准设计](docs/leader/designs/V0.3/S2-design.md)
- [V0.3/S2审查计划](docs/reviewer/reviews/V0.3/S2-review.md)
- [V0.3/S2完成报告](docs/leader/reports/V0.3/S2-report-003.md)

### 已完成的V0.3/S1

S1 Completed，Builder001、Reviewer001 PASS、Leader003齐备；已合并PR7，main与v0.3-s1标签核验9971818b，含实现0780b7d；当前S2准备见上节。

- [V0.3/S1批准设计](docs/leader/designs/V0.3/S1-design.md)
- [V0.3/S1审查计划](docs/reviewer/reviews/V0.3/S1-review.md)
- [V0.3/S1完成报告](docs/leader/reports/V0.3/S1-report-003.md)

### 已完成的V0.2

V0.2/S3 已批准，分支 `codex/v0.2-s3`，基线 V0.2-S3-D1 Approved，独立Reviewer001结论PASS，Leader003收尾，S3及V0.2开发范围Completed。S3已合并，远端main与v0.2-s3标签peeled均核验为7821b25；当前V0.3/S1完成状态见上节。

- [S3 已批准设计](docs/leader/designs/V0.2/S3-design.md)
- [S3 审查计划](docs/reviewer/reviews/V0.2/S3-review.md)
- [S3 准备决策](docs/leader/reports/V0.2/S3-report-001.md)
- [S3 批准登记](docs/leader/reports/V0.2/S3-report-002.md)
- [S3 独立审查](docs/reviewer/reports/V0.2/S3-report-001.md)：PASS，Debug快速10/10、Release全11/11及原生产60秒验证。
- [S3与V0.2完成报告](docs/leader/reports/V0.2/S3-report-003.md)：Completed；保留当时收尾记录，S3现已合并并标记v0.2-s3。

### 已完成的 V0.2/S2

V0.2/S2 已批准，分支 `codex/v0.2-s2`，V0.2-S2-D1 Approved，当前 Completed，独立 Reviewer001 PASS、Leader003 收尾。S2 已完成 UDP flow 绑定与回复及必要验收；这是S2历史结果，当前S3进度见上节。

- [S2 已批准设计](docs/leader/designs/V0.2/S2-design.md)
- [S2 审查计划](docs/reviewer/reviews/V0.2/S2-review.md)
- [S2 准备决策报告](docs/leader/reports/V0.2/S2-report-001.md)
- [S2 批准登记](docs/leader/reports/V0.2/S2-report-002.md)
- [S2 独立审查](docs/reviewer/reports/V0.2/S2-report-001.md)：PASS，独立两模式 9/9 与实际产品负向验证。
- [S2 完成报告](docs/leader/reports/V0.2/S2-report-003.md)：Completed，保留当时S3未开始的历史状态。

### 已完成的 V0.2/S1

V0.2/S1 已批准（分支 `v0.2-s1`，V0.2-S1-D1 Approved），当前 Completed，独立 Reviewer002 复审 PASS（Debug/Release 各 7/7），F-001 已关闭，Leader 已完成最终状态同步。S1 聚焦调度抽象和配置扩展，UDP 转发留在 S2。

- [V0.2/S1 已批准设计](docs/leader/designs/V0.2/S1-design.md)
- [V0.2/S1 审查计划](docs/reviewer/reviews/V0.2/S1-review.md)
- [V0.2/S1 准备报告](docs/leader/reports/V0.2/S1-report-001.md)
- [V0.2/S1 批准登记](docs/leader/reports/V0.2/S1-report-002.md)
- [V0.2/S1 首轮审查](docs/reviewer/reports/V0.2/S1-report-001.md)：保留首轮 FAIL，F-001 已关闭。
- [V0.2/S1 复审报告](docs/reviewer/reports/V0.2/S1-report-002.md)：PASS，F-001 Closed。
- [V0.2/S1 完成报告](docs/leader/reports/V0.2/S1-report-003.md)：Completed；保留 S1 收尾时的历史状态。

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
- 仅支持静态数字 IPv4 TCP/UDP 端点，不支持 DNS、IPv6 或热加载。
- TCP 按连接、UDP 按 flow 静态轮询；默认 health_check=off，可显式启用 TCP 握手资格过滤，无失败切换、UDP 可靠交付或性能承诺。UDP 仅用于受控实验网络，不提供源地址反欺骗或公网开放 relay 防护。
- WSL2 不适合作为 XDP/eBPF native mode 的最终性能验证环境。
- XDP/eBPF 阶段计划使用云服务器进行功能验证和收尾；性能结论必须标注云环境限制。
- 本项目主线聚焦 L4 负载均衡与 XDP/eBPF，不包含 DPDK 实现。

详细长期技术债和风险将记录到 `TECH-DEBT-TRACKER.md`。

## 许可证

暂未指定。

## V0.3/S1 健康检查

配置可增加 `health_check=tcp_connect`，缺省为 `off`，旧示例无需修改。启用时 Unknown 初始拒绝新 TCP 会话/UDP flow，连续2次成功开放、3次失败摘除，完成后间隔1s、超时1s。ready 只表示监听就绪。全部不可选不 fallback，恢复后自动允许新业务；已有 TCP/UDP 绑定不因健康摘除而迁移或关闭。

TCP 握手不证明应用业务可用。UDP 必须在后端相同 IP/数字端口提供有代表性的 TCP 健康端点，并由操作员保证信号代表 UDP；UDP-only 服务应保持 off。探活产生空 TCP 连接，本机资源不足也可能保守摘除并标识 local_error。完整阈值、日志和边界见 [健康规格](docs/specs/health-check.md)。

测试构建新增 Python3 标准库 fixture，无额外包；生产 `BUILD_TESTING=OFF` 无 Python 依赖。S1验收时原11项保留、新增4项，总15项；Debug快速 `ctest --test-dir <Debug目录> -LE udp_long` 为14项，Release完整 `ctest --test-dir <Release目录>` 为15项。单独健康测试可用 `ctest --test-dir <构建目录> -R v03_health`，真实产品用默认时序。原始执行证据见 [Builder报告](docs/builder/reports/V0.3/S1-report-001.md)。

Reviewer独立验收 PASS：Debug快速14/14、Release完整15/15，Production两协议健康/普通uid CLI与三类Release负向通过，见 [审查报告](docs/reviewer/reports/V0.3/S1-report-001.md)。Leader收尾完成，S1 Completed，见 [完成报告](docs/leader/reports/V0.3/S1-report-003.md)；S2/S3及V0.3开发范围均已完成。

## V0.3/S2 指标输出

配置可增加 `metrics=stderr`，默认off，与health_check独立。原stderr日志中每秒增加 `metrics ` 前缀JSON快照，并在ready和清理后final/error各输出相应快照。计数包括活动会话/flow、已成功向内核提交的双向payload字节/UDP包、拒绝/丢弃/错误/超时以及后端健康；不保证对端收到，探活不算业务量，UDP零长包也计一包。

同步stderr可能拖慢业务及停止；堵塞管道不保证及时退出，关闭管道可能SIGPIPE终止。建议写本地普通文件并外部管理日志。写失败/短写禁用后续指标，半行不保证修复；仅解析完整 `metrics ` JSON行，详见 [指标规格](docs/specs/metrics.md)。无新端口/线程/生产依赖。新增5项metrics测试，S2阶段总20项；S3增加3项后当前23项（Debug快速22、Release完整23含原60s expiry）；单独运行 `ctest --test-dir build -R v03_metrics`。

当前实现证据：[V0.3/S2 Builder002](docs/builder/reports/V0.3/S2-report-002.md)，两项首轮问题已由 [Reviewer002 PASS](docs/reviewer/reports/V0.3/S2-report-002.md) 独立确认关闭；Debug19/19、Release20/20、Production及五类负向通过，Leader收尾完成，S2 Completed，见 [完成报告](docs/leader/reports/V0.3/S2-report-003.md)。历史 [Builder001](docs/builder/reports/V0.3/S2-report-001.md) / [Reviewer001 FAIL](docs/reviewer/reports/V0.3/S2-report-001.md) 保留：UDP建flow异常漏drop和正式fd结束断言缺失；两项均已关闭，S3及V0.3开发范围已完成。

## V0.3/S3 故障场景验证

新增双backend TCP/UDP部分摘除、全不可选、分步恢复和真实TCP连接拒绝场景，以独立nonce和指标账本验证旧绑定、轮询及最终清理。使用 `bash tests/v03_validate.sh build-release/bin/l4lb validation-v03`；网络隔离、构建、工具负向及证据说明见 [公开运行手册](docs/runbooks/local-v0.3-validation.md)，逐条版本标准见 [验收矩阵](docs/specs/v0.3-acceptance.md)。Builder与 [Reviewer001 PASS](docs/reviewer/reports/V0.3/S3-report-001.md) 齐备：独立Debug22/22、Release23/23及公开重复/并行/工具负向通过。Leader003最终同步完成，S3及V0.3开发范围Completed；现已合并并标记v0.3-s3（281db01），当前V0.4/S1 Completed，Reviewer002复审PASS，F-001 Closed，Leader003收尾完成；V0.4/S2 Completed，Reviewer001 PASS、Leader003收尾；S3当前状态见本文开头。

## V0.4/S1 Benchmark 工具

公开[测量方法](docs/benchmarks/methodology.md)、[报告模板](docs/benchmarks/report-template.md)与[工具验证样本](docs/benchmarks/tool-validation-sample.md)独立于角色私有文档。使用 Python3 标准库；产品 Release/Production 显式二进制路径均可运行，健康检查与 metrics 关闭，一个 echo backend。示例从仓库根目录执行，输出目录须为新目录：

```bash
B="$PWD/.stage-tmp/benchmark-example"
mkdir -p "$B/tmp" "$B/cache" "$B/pycache"
export TMPDIR="$B/tmp" TMP="$B/tmp" TEMP="$B/tmp" XDG_CACHE_HOME="$B/cache" PYTHONPYCACHEPREFIX="$B/pycache"
cmake -S . -B "$B/Production" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$B/Production" -j4
python3 tests/benchmark_runner.py --program "$B/Production/bin/l4lb" --protocol tcp --mode paired --output "$B/tcp-default"
python3 tests/benchmark_runner.py --program "$B/Production/bin/l4lb" --protocol udp --mode paired --output "$B/udp-default"
```

默认256-byte payload、1 client、预热1秒/测量5秒/timeout1秒、UDP总目标1000pps、3次重复，direct/proxy顺序交替。每run保存身份、环境、配置、原始sampled RTT、独立进程资源、计数、valid与清理证据；goodput只计成功回显payload一次并包含有界drain。低RTT须并列丢失率，valid不表示速度达标。WSL/loopback共享CPU和Python工具限制使这些数据不能当作硬件极限、S2优化成果或S3正式性能报告。

## V0.4/S2 资源与有界停止

TCP两向各64KiB改为固定容量ring，保持32KiB低水位、64KiB预算和字节顺序。第一次消费INT/TERM关闭listener、停止新recv/连接/健康maintenance，仅尝试发送已在用户态pending的队列，固定1s截止；第二次消费信号立即强制关闭。空队列不等EOF；截止/force不伪称全部在途数据送达。TCP/UDP关闭都先移除身份并释放fd，再独立通知；析构不因通知异常中断清理。UDP仍立即停止、无排空/重传。

公开[生命周期运行手册](docs/runbooks/local-v0.4-lifecycle-validation.md)包含三构建、ASan/UBSan、Production真实信号/健康指标、并行/空格路径、目标负向及S1工具兼容命令。短入口：

```bash
ctest --test-dir build-release -L v04_lifecycle --output-on-failure
python3 tests/v04_shutdown_product.py --program "$PWD/build-release/bin/l4lb" --output "$PWD/.stage-tmp/shutdown-example"
```

新增TCP生命周期日志给出首次消费信号时的双向pending及steady-clock截止；最终摘要明确“已尝试有界排空用户态待发队列，不保证在途数据送达”。同步stderr阻塞、回调不返回及OS调度不受1s逻辑时限保证，无新配置字段。本阶段不承诺性能提升，S3正式报告见下一节。

## V0.4/S3 固定跨版本报告

[正式用户态报告](docs/benchmarks/reports/v0.4-user-space.md)比较固定S1/S2产品，用同一工具串行运行TCP/UDP各两种并发、三轮、direct/proxy，共48run；包含可跟踪原始包、所有三次值与波动/瓶颈边界。[版本六标准矩阵](docs/specs/v0.4-acceptance.md)区分自测、独立审查与最终收尾。

完整普通clone中可离线重算公开数据，无需本机历史binary或私有角色目录：

```sh
B="$PWD/.stage-tmp/report-recompute"
mkdir -p "$B/tmp" "$B/cache" "$B/pycache"
export TMPDIR="$B/tmp" TMP="$B/tmp" TEMP="$B/tmp" XDG_CACHE_HOME="$B/cache" PYTHONPYCACHEPREFIX="$B/pycache"
python3 tests/v04_benchmark_compare.py recompute --package docs/benchmarks/reports/v0.4-user-space.raw.json.gz --output "$B/output"
```

输出目录必须新建；需包含两个固定tag的Git对象。重新导出/构建/测量和短验证命令见[方法](docs/benchmarks/methodology.md#v04s3-固定跨版本比较)。新增CTest标签 `v04_compare` 只运行纯校验，48run不进默认回归。性能无提升门槛，不承诺饱和容量或把S1到S2全部差值归因给ring。
