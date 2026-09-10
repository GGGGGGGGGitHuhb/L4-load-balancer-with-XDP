# L4 Load Balancer with XDP

C++20 用户态四层负载均衡实验项目：单线程 LT epoll TCP 双向字节流、UDP flow 固定绑定、轮询、可选 TCP 握手健康检查与 stderr 指标。XDP/eBPF 是后续路线，当前无需内核模块、数据库或外部服务。

## 当前状态

V0.1—V0.4 用户态开发范围已完成；V1.0/S1 已冻结[用户可见行为](docs/specs/v1.0-user-visible-contract.md)。V1.0/S2 文档与验收补齐已完成（2026-09-10）：独立Debug快速30/30、Release完整31/31、最短示例与公开文档验证通过，Leader已收尾。S3独立发布前审查PASS且Leader已收尾，**S1/S2/S3及V1.0用户态开发范围Completed，本轮候选发布就绪，实际发布未执行**（2026-09-10）。S3独立Debug30/30、Release31/31含长expiry及原TCP/UDP示例通过，不自动启动未来XDP阶段。本轮候选及三种状态见[公开发布前审查](docs/specs/v1.0-release-review.md)。阶段证据与待验事项见[公开 V1.0 验收索引](docs/specs/v1.0-acceptance.md)；历史版本结果分别保留在[V0.1](docs/specs/v0.1-acceptance.md)、[V0.2](docs/specs/v0.2-acceptance.md)、[V0.3](docs/specs/v0.3-acceptance.md)、[V0.4](docs/specs/v0.4-acceptance.md)矩阵中，不是当前测试数量。

## 环境与快速构建

Linux/WSL2、支持 C++20 的 Clang++、CMake ≥3.20、Ninja。默认测试及benchmark使用 Python3 标准库，无 pip 包下载；**生产 `BUILD_TESTING=OFF` 不依赖 Python**。运行示例需要 Bash，UDP演示另用 coreutils/ripgrep。正常Linux回环不需要root或外网。

在仓库根执行（每种构建用独立目录）：

```bash
mkdir -p .stage-tmp/readme/tmp .stage-tmp/readme/cache .stage-tmp/readme/pycache
export TMPDIR="$PWD/.stage-tmp/readme/tmp" TMP="$PWD/.stage-tmp/readme/tmp" TEMP="$PWD/.stage-tmp/readme/tmp"
export XDG_CACHE_HOME="$PWD/.stage-tmp/readme/cache" PYTHONPYCACHEPREFIX="$PWD/.stage-tmp/readme/pycache"
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/bin/l4lb --help
./build/bin/l4lb --check-config configs/example.conf
```

配置检查输出 `配置有效：TCP，后端数量=2` 加换行；它只做静态校验，不创建socket或证明backend可达。所有程序位于构建目录的 `bin/`。

## TCP 最短运行路径

三个终端依次执行，下列固定端口须未被占用：

```bash
# 终端1
./build/bin/tcp_echo_backend 9001
# 终端2
./build/bin/tcp_echo_backend 9002
# 终端3
./build/bin/l4lb --run configs/example.conf
```

终端3输出 `TCP 服务已启动：127.0.0.1:8080` 后，第四个 Bash 终端执行：

```bash
exec 3<>/dev/tcp/127.0.0.1/8080
printf 'hello S2\n' >&3
IFS= read -r reply <&3
printf '%s\n' "$reply"
exec 3<&- 3>&-
```

预期 `hello S2`。在三个服务终端分别 Ctrl+C 结束；代理消费SIGINT/SIGTERM后只尝试在固定1秒内发送已有用户态pending，不保证网络在途数据送达。echo是测试构建生成的单连接顺序fixture，不是生产依赖。

自动真实产品验证（动态端口、有界进程清理）：

```bash
./build/bin/tcp_product_smoke ./build/bin/l4lb ./build/bin/tcp_echo_backend ./build/product-evidence
```

预期退出0、PASS及独立证据目录；[TCP手册](docs/runbooks/local-tcp-validation.md)解释字节、半关闭、故障、重复验证和资源回收。

## UDP 最短运行路径

在同一正常回环环境执行公开单shell演示；输出目录保存新一轮证据，18080—18085须未被占用：

```bash
bash tests/udp_manual_demo.sh ./build/bin/l4lb ./build/udp-manual-evidence 18080
```

脚本实际生成UDP配置、执行`--check-config`（输出`配置有效：UDP，后端数量=2`）、启动两个标记后端和原产品，验证wildcard源地址、固定flow、零长包、故障后显式新请求、恢复及占端口失败；预期最后PASS、所有自有PID退出和六端口可重绑。示例后端的XOR标记仅用于验证，产品不修改payload。手动多终端参数及逐步输出见[UDP手册](docs/runbooks/local-udp-validation.md#完整手动流程可直接运行的单-shell-版本)。UDP仅尽力转发，停止关闭flows，无排空保证。

## 配置、健康与指标

[配置规格](docs/specs/config-schema.md)冻结六键：必填`listen`一次、`backend` 1—256个有序不同端点；可选默认`protocol=tcp`、`scheduler=round_robin`、`health_check=off`、`metrics=off`。只支持数字IPv4:端口，键值大小写敏感，不搜索默认文件、不读环境覆盖、不写回配置。文件上限65536字节，普通文件/指向普通文件的符号链接允许。

- CLI成功0、文件/配置/服务错误1、用法错误2；成功/check/ready写stdout，诊断和指标写stderr。ready只表示监听就绪，不保证backend可达或Healthy。
- [健康检查](docs/specs/health-check.md)：`tcp_connect`只证明TCP握手，Unknown初态，连续2成功开放、3失败摘除。无Healthy拒绝新业务，已有TCP/UDP绑定不迁移。UDP**仅在启用tcp_connect时**需要同IP/端口且代表UDP服务的TCP健康端点。
- [metrics](docs/specs/metrics.md)：`stderr`与健康开关独立，schema=1；ready/periodic/final/error快照，字节计成功提交内核、不代表送达。stderr混合业务日志，整体不是JSONL。
- 同步stdout/stderr可能阻塞业务和退出；指标写失败后禁用、可能半行，SIGPIPE/强杀可能无尾快照。建议本地普通文件并外部管理轮转。

## 测试与生产构建

当前CTest注册 **31项**；Debug快速 **30项**，Release完整 **31项**（包含约61秒的原60秒UDP expiry）。由`ctest -N`核对，不把历史阶段数字当当前结果。

```bash
ctest --test-dir build -N
ctest --test-dir build -LE udp_long --output-on-failure
cmake -S . -B build-release -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j4
ctest --test-dir build-release --output-on-failure
cmake -S . -B build-production -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-production -j4
```

`s2`/`s3`标签来自历史V0.1 TCP阶段，不是当前V1.0的全套；`udp_fast`/`udp_long`来自V0.2；`v03_health`、`v03_metrics`可作为`-R`筛选；`v03_system`为故障联合验证，`v04_lifecycle`为停止/资源，`v04_bench_smoke`为短benchmark，`v04_compare`为离线比较工具，`v10_contract`为稳定契约及成对端口夹具。

普通UID会测试不可读文件，root会明确跳过。如果受限Agent或WSL路由阻止回环测试，可在**独立**`unshare --user --map-root-user --net bash`中先`ip link set lo up`再运行网络命令；这是受限环境的执行路线，可能需要执行器窄授权，不是产品root要求。不修改宿主接口/代理/sysctl。该namespace的UID0不算权限证据，另在原普通UID执行`ctest --test-dir build -R '^cli_integration$' --output-on-failure`。

## Benchmark：当前smoke与历史报告

当前binary短验证（每协议一组低负载direct/proxy，不作正式性能结论；每次使用不存在的输出目录）：

```bash
python3 tests/benchmark_runner.py --program ./build-production/bin/l4lb --protocol tcp --mode paired --warmup 0 --duration 1 --repeats 1 --rate 100 --output .stage-tmp/readme/tcp-smoke
python3 tests/benchmark_runner.py --program ./build-production/bin/l4lb --protocol udp --mode paired --warmup 0 --duration 1 --repeats 1 --rate 100 --output .stage-tmp/readme/udp-smoke
python3 tests/v04_benchmark_compare.py recompute --package docs/benchmarks/reports/v0.4-user-space.raw.json.gz --output .stage-tmp/readme/published-recomputed
```

[正式V0.4报告](docs/benchmarks/reports/v0.4-user-space.md)比较固定历史v0.4-s1与v0.4-s2的48run；[机器摘要](docs/benchmarks/reports/v0.4-user-space.json)可由上面公开包离线重算，当前smoke不能更新或替代它。recompute不需要产品build或私有角色目录；重新构建历史产品则需要完整clone中的对应Git标签/对象，不能用缺历史对象的源文件导出冒充完整clone。详细统计口径、环境/采样边界及可选正式复现见[测量方法](docs/benchmarks/methodology.md)。

## 公开文档与限制

- [架构](ARCHITECTURE.md)、[路线](ROADMAP.md)、[变更记录](CHANGELOG.md)、[技术债](TECH-DEBT-TRACKER.md)。
- [TCP语义](docs/specs/tcp-forwarding-semantics.md)、[UDP flow](docs/specs/udp-flow-table.md)、[调度](docs/specs/scheduler.md)、[稳定行为契约](docs/specs/v1.0-user-visible-contract.md)。
- [故障联合验证](docs/runbooks/local-v0.3-validation.md)、[资源与有界停止](docs/runbooks/local-v0.4-lifecycle-validation.md)、[历史开发环境](docs/runbooks/local-dev-env.md)。
- [V1.0验收索引](docs/specs/v1.0-acceptance.md)区分S1历史组合证据、S2本轮验证和S3待验。

仅支持静态IPv4，无DNS/IPv6/热加载/失败换后端重试或UDP可靠交付。容量默认1024，完整产品1024满载未经承诺；同步输出、内核缓冲等不包含在用户态pending容量中。UDP用于受控实验网络，无公网开放relay或源地址反欺骗防护。WSL/Python生成器/共享CPU测量不代表物理网卡或native XDP上限；后续XDP功能验证路线见技术债TD-003，主线不包含DPDK。

`docs/leader/`、`docs/builder/`、`docs/reviewer/`及根AGENTS属于本地治理记录，不随普通clone分发；它们不是运行和公开验收的必读入口。ROADMAP中的未来设计路径是计划位置，不代表文件或能力已存在。公开链接检查可运行`python3 tests/docs_links.py .`，仅检查根与docs Markdown的本地链接和锚点，不联网扫外链。

## 许可证

暂未指定。

## 提交前格式检查

仓库提供 `.githooks/pre-commit`。每个新 clone 启用一次：

```sh
git config core.hooksPath .githooks
```

需要 Bash、Git、clang-format 和常用 GNU 命令；本次存量格式核验使用 clang-format 18.1.3。规则来自仓库 `.clang-format`（Google、`SeparateDefinitionBlocks: Always`），函数定义之间保留空行。hook 不另行覆盖风格。

每次提交遍历 **index 中全部追踪的普通 C/C++ 文件**，包括本次没有暂存变化的文件和已暂存的新文件。处理扩展名为 `.c/.cc/.cpp/.cxx/.c++/.h/.hh/.hpp/.hxx/.h++/.ipp/.tpp/.inl/.C/.H/.cu/.cuh`；Python、JSON、Markdown、配置、历史压缩证据等不交给 clang-format，未追踪文件、符号链接和submodule不修改。

hook 格式化工作树，并独立读取暂存blob检查格式，**从不运行 git add 或改写 index**。工作树发生格式变化，或暂存版本仍未符合格式时，提交返回非0：先检查 `git diff` 与 `git diff --cached`，选择性暂存后重新提交。部分暂存的逻辑改动始终由用户选择；即使工作树已经格式正确，暂存版本不合规仍会拒绝。`.clang-format` 的工作树与暂存版本不同也会拒绝，避免用未提交的规则检查提交。

已暂存删除的文件不参与；未暂存删除保持缺失，只检查仍在index中的原blob。文件路径按NUL读取，支持空格和换行。缺少clang-format或格式器出错时停止提交；工作树格式改动保留供检查，暂存内容保持原样。临时文件位于已忽略的 `.stage-tmp/git-hooks` 并在退出时清理。

可在新的临时目录中实测hook（只对测试仓库做commit，不提交当前仓库）：

```sh
B="$PWD/.stage-tmp/format-hook-check"
mkdir -p "$B/tmp" "$B/cache" "$B/pycache"
export TMPDIR="$B/tmp" TMP="$B/tmp" TEMP="$B/tmp" XDG_CACHE_HOME="$B/cache" PYTHONPYCACHEPREFIX="$B/pycache"
python3 tests/format_hook_test.py --output "$B/evidence"
```

历史benchmark报告和数据包的源码指纹对应当时被测版本，格式维护不会重写它们，也不据此重新宣称性能结果。
