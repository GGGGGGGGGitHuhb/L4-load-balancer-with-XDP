# L4 Load Balancer with XDP

一个用于学习和验证四层负载均衡的 C++20 实验项目。可以运行 TCP/UDP 用户态代理，观察轮询、健康检查与流绑定；也可以在 Linux 隔离网络中运行 XDP UDP DSR，对比两条转发路径的行为和成本。

用户态与 XDP 是两个独立程序：`l4lb` 通过 socket 转发，`l4lb-xdp` 管理内核 XDP 程序与配置。项目已完成 V0.1—V1.2 的开发与验收，以及 R1—R4 编码规范重构；版本变化与阶段标签见 [CHANGELOG](CHANGELOG.md)。

## 已实现能力

| 路径 | 能力 |
|---|---|
| 用户态 TCP | 单线程 LT epoll 双向转发、轮询、背压、半关闭与连接超时 |
| 用户态 UDP | 按 flow 固定绑定后端、轮询、空闲过期与回复源地址保持 |
| 用户态控制 | 可选 TCP 握手健康检查、stderr 指标、信号停止与资源清理 |
| XDP | PASS、map 同步与计数、静态 UDP DSR、运行期 UDP DSR |
| 运行期 DSR | 不可变配置快照、SIGHUP 重载、UDP nonce echo 健康摘除与恢复 |

DSR 根据五元组选择后端，只改写二层 MAC，由后端直接向客户端回包。它需要配置 VIP、后端网络和回程；具体拓扑见[架构](ARCHITECTURE.md#current-source-index)。

## TCP 最短运行路径

### 环境与快速构建

在 Linux/WSL2 的 Bash 中，从仓库根目录执行。需要支持 C++20 的 Clang++、CMake ≥3.20、Ninja；默认测试构建另需 Python3 标准库，无需 pip 包。正常 Linux 回环运行无需 root。

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/bin/l4lb --check-config configs/example.conf
```

预期输出 `配置有效：TCP，后端数量=2`。这只检查配置，不探测后端是否可达。程序生成在构建目录的 `bin/` 中。

### 启动与请求

确保 8080、9001、9002 端口未被占用，依次在三个终端执行：

```bash
# 终端 1：示例后端 A
./build/bin/tcp_echo_backend 9001
# 终端 2：示例后端 B
./build/bin/tcp_echo_backend 9002
# 终端 3：代理
./build/bin/l4lb --run configs/example.conf
```

代理输出 `TCP 服务已启动：127.0.0.1:8080` 后，在第四个 Bash 终端发送请求：

```bash
exec 3<>/dev/tcp/127.0.0.1/8080
printf 'hello\n' >&3
IFS= read -r reply <&3
printf '%s\n' "$reply"
exec 3<&- 3>&-
```

预期收到 `hello`。完成后在三个服务终端分别按 Ctrl+C。示例 echo 后端由测试构建生成，实际使用时替换为自己的服务。停止会尽力发送用户态待发数据，不保证在途数据送达。

更多半关闭、故障和自动验证步骤见 [TCP 运行手册](docs/runbooks/local-tcp-validation.md)。

## UDP 与 XDP

### UDP 演示

默认构建完成后运行；需要 Bash、coreutils 和 ripgrep，18080—18085 端口须空闲，证据目录使用新路径：

```bash
bash tests/udp_manual_demo.sh ./build/bin/l4lb ./build/udp-manual-evidence 18080
```

脚本启动两个示例后端和代理，验证 flow 绑定、回复源地址、零长报文及故障恢复，预期最终输出 PASS 并清理自有进程。逐步操作见 [UDP 运行手册](docs/runbooks/local-udp-validation.md)。

### XDP 入口

XDP 默认关闭。BPF 对象需要支持 BPF 的 Clang 与 Linux UAPI；独立加载工具另需 libbpf ≥1.0 开发文件。构建、挂载和真实转发需要不同的依赖与权限，按以下手册操作：

- [构建 BPF 对象](docs/runbooks/xdp-build.md)、[构建与运行加载工具](docs/runbooks/xdp-loader.md)。
- [静态 UDP DSR 配置与部署](docs/specs/xdp-udp-dsr.md)。
- [运行期 DSR 启动与重载](docs/specs/xdp-runtime-control.md#启动和重载)。
- [Linux/WSL2 环境](docs/runbooks/linux-xdp-env.md)、[隔离网络验证](docs/runbooks/xdp-validation.md)。

`l4lb-xdp` 提供 pass、maps、udp-dsr、udp-runtime 四种 profile；它不会自动回退到用户态代理，也不提供 TCP XDP 转发。

## 配置

下面是 `configs/example.conf` 中实际使用的 TCP 配置：

```ini
listen=127.0.0.1:8080
backend=127.0.0.1:9001
backend=127.0.0.1:9002
```

可选项默认值为 `protocol=tcp`、`scheduler=round_robin`、`health_check=off`、`metrics=off`。切换 UDP 使用 `protocol=udp`；开启健康检查使用 `health_check=tcp_connect`，开启指标使用 `metrics=stderr`。

- 端点使用数字 IPv4 与端口，配置键大小写敏感；必须指定配置文件，不读取环境覆盖或写回文件。完整格式见[配置规格](docs/specs/config-schema.md)。
- 用户态健康检查只证明 TCP 握手。UDP 启用该模式需要同 IP/端口且能代表 UDP 服务的 TCP 健康端点；已有连接与 flow 不随健康变化迁移。见[健康规格](docs/specs/health-check.md)。
- 指标写 stderr，与业务日志混合；stdout 的 ready 只说明监听就绪。同步输出可能阻塞，计数字节不等于实际送达。见[指标规格](docs/specs/metrics.md)。

## 测试与生产构建

在正常回环环境以普通用户运行完整测试：

```bash
ctest --test-dir build --output-on-failure
```

完整测试含约 60 秒的 UDP 空闲过期验证。测试数量以 `ctest --test-dir build -N` 为准；真实内核 XDP 验证需另按手册准备特权隔离环境。

生产构建设置 `BUILD_TESTING=OFF`，不需要 Python，也不生成示例后端。构建命令、快速测试筛选及格式 hook 见[开发与维护手册](docs/runbooks/development-maintenance.md)。

## Benchmark：当前smoke与历史报告

已有两份可复核的正式报告：

- [用户态 TCP/UDP 报告](docs/benchmarks/reports/v0.4-user-space.md)：固定 V0.4/S1 与 S2 对照，附[测量与重算方法](docs/benchmarks/methodology.md)。
- [UDP/XDP 报告](docs/benchmarks/reports/v1.2-xdp.md)：direct、用户态 UDP、静态与运行期 DSR 的六路径及控制面成本，附[测量与重算方法](docs/benchmarks/v1.2-methodology.md)。

生产构建后，可运行当前二进制的低负载短验证。每次使用不存在的输出目录：

```bash
python3 tests/benchmark_runner.py --program ./build-production/bin/l4lb --protocol tcp --mode paired --warmup 0 --duration 1 --repeats 1 --rate 100 --output build-production/tcp-smoke
python3 tests/benchmark_runner.py --program ./build-production/bin/l4lb --protocol udp --mode paired --warmup 0 --duration 1 --repeats 1 --rate 100 --output build-production/udp-smoke
```

短验证不会更新或替代历史正式报告。WSL2/veth、Python 发生器与共享 CPU 的测量结果不代表物理 NIC 线速或产品上限；完整采样条件和数据来源以报告为准。

## 使用边界

- 当前只支持 IPv4 与 round-robin；没有 DNS、IPv6、NAT、失败后自动换后端重试或 UDP 可靠交付。
- 用户态配置在启动时加载；静态 DSR 修改需重启，运行期 DSR 支持 SIGHUP 重载。后端集合变化可能重映射已有 DSR UDP 流。
- 用户态默认连接/flow 容量为 1024，完整产品满载能力未经承诺。UDP 面向受控实验网络，没有公网 relay 的源地址反欺骗防护。
- 验证主要基于 Linux/WSL2 回环与隔离 veth；其他平台、物理网卡能力和多核扩展需要另行验证。

## 文档导航

- 理解实现：[架构与源码索引](ARCHITECTURE.md#current-source-index)、[TCP 语义](docs/specs/tcp-forwarding-semantics.md)、[UDP flow](docs/specs/udp-flow-table.md)、[调度](docs/specs/scheduler.md)。
- 检查行为：[稳定行为契约](docs/specs/v1.0-user-visible-contract.md)、[故障联合验证](docs/runbooks/local-v0.3-validation.md)、[停止与资源验证](docs/runbooks/local-v0.4-lifecycle-validation.md)。
- 开发维护：[维护手册](docs/runbooks/development-maintenance.md)、[版本变化](CHANGELOG.md)。
- 了解后续：[路线图](ROADMAP.md)、[技术债与风险](TECH-DEBT-TRACKER.md)。

## 许可证

暂未指定许可证。
