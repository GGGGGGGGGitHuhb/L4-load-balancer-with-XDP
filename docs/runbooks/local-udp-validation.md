# 本地 UDP 验证

适用 V0.2/S3。普通 clone 只需源码、tests 工具和本手册；不依赖忽略的角色报告或既有证据目录。构建依赖 Linux/WSL2、Clang/C++20、CMake≥3.20、Ninja；手动工具额外使用 Python3 标准库，单 shell 演示脚本使用 Bash、coreutils、ripgrep。无需下载 Python 包。

## 构建、测试数量与网络环境

在仓库根目录执行；所有路径可替换为自己的独立目录。

```bash
mkdir -p build/task-tmp build/task-cache
export TMPDIR="$PWD/build/task-tmp" TMP="$PWD/build/task-tmp" TEMP="$PWD/build/task-tmp"
export XDG_CACHE_HOME="$PWD/build/task-cache" PYTHONPYCACHEPREFIX="$PWD/build/task-cache/pycache"
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cmake -S . -B build-release -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build -N
```

注册 **11 项**，保留 S2 九项，新增 `v02_udp_system` 和 `v02_udp_expiry`。Debug 常规执行 10 项快速测试；Release 执行全部 11 项，长项只需一次。

若允许真实本机 loopback，直接执行下列 CTest 命令即可。当前已知部分执行环境把 127/8 经 loopback0 转发，会丢弃超过1472字节UDP；在临时 user/net namespace 内启用独立 lo 可排除该环境问题，不改宿主接口、路由、代理或限额：

```bash
unshare --user --map-root-user --net sh -c 'ip link set lo up && ctest --test-dir build -LE udp_long'
unshare --user --map-root-user --net sh -c 'ip link set lo up && ctest --test-dir build-release'
# netns 内 uid=0，普通文件权限用例另在原普通用户环境补齐：
ctest --test-dir build -R 'config_unit|cli_integration|v02_scheduler_unit'
ctest --test-dir build-release -R 'config_unit|cli_integration|v02_scheduler_unit'
```

预期依次执行 10/10、11/11、3/3、3/3，退出0。namespace 不可用时应在具有正常本地回环的环境执行，不能缩小最大包或把环境失败当通过。未修改生产源代码、默认时限或配置，不需要 root 修改系统网络。

## 快速组与生产默认长项

`udp_fast` 只包含原 UDP 基础产品项及 P1/P2 增量系统项，不包含长项：

```bash
unshare --user --map-root-user --net sh -c 'ip link set lo up && ctest --test-dir build -L udp_fast --repeat until-fail:3'
unshare --user --map-root-user --net sh -c 'ip link set lo up && ctest --test-dir build-release -L udp_fast --repeat until-fail:3'
```

每组两项各连续3次，退出0。独立直接入口为：

```bash
./build-release/bin/udp_product_test ./build-release/bin/l4lb ./build-release/udp-evidence
./build-release/bin/udp_product_test ./build-release/bin/l4lb ./build-release/udp-evidence system
./build-release/bin/udp_product_test ./build-release/bin/l4lb ./build-release/udp-evidence expiry
```

上述网络命令也应在同一个正常回环/独立 namespace 内运行。路径可含空格，只需 shell 引号；程序使用独立 argv，证据根下每次原子创建不同 `run-XXXXXX`。成功退出0，错误退出1，用法错误退出2；配置、子进程输出、PID、端口重绑和 `result.txt` 原样保留。

- **P1/system**：真实产品 wildcard，同一客户端对两个目的IP分别A/B，重发第一目标仍A；同后端两客户端反序回复严格归属，第三socket伪造无泄漏且合法控制包正常。每包核对完整字节、源IP/端口及额外包。
- **P2/system**：实际关闭A，用对应flow id的真实ECONNREFUSED日志作同步屏障；失败旧包客户端无回复、B无旧nonce；同key显式新包才选择B。恢复原A端口后新key选择A，旧B保持B。这不是探活、重试或透明切换。
- **P3/expiry**：原产品先完成A往返，保持同一客户端socket/目标，静默至少60.5秒后新nonce在B往返，A不得收到新nonce。内部总截止75秒，CTest80秒；单项约61秒。不向产品注入短时限、不发保活探针、不改系统时钟。Release全套已经包含它，不需要再重复命令。Debug/快速重复/并行/Production不要求重跑长项。

并行快速组可在已启用lo的同一个 namespace shell 内执行：

```bash
(./build-release/bin/udp_product_test ./build-release/bin/l4lb ./build-release/parallel-A &&
 ./build-release/bin/udp_product_test ./build-release/bin/l4lb ./build-release/parallel-A system) > ./build-release/parallel-A.log 2>&1 &
pid_a=$!
(./build-release/bin/udp_product_test ./build-release/bin/l4lb ./build-release/parallel-B &&
 ./build-release/bin/udp_product_test ./build-release/bin/l4lb ./build-release/parallel-B system) > ./build-release/parallel-B.log 2>&1 &
pid_b=$!
wait "$pid_a"; rc_a=$?
wait "$pid_b"; rc_b=$?
printf 'A=%s B=%s\n' "$rc_a" "$rc_b"
```

预期A=0 B=0。只持有自己的PID和动态端口。启动仅在确证bind冲突且旧进程已回收时有界重试；缺ready/提前退出/数据错误不重复掩盖。cleanup错误必须使总结果非零，外层124不是断言证据。

## 完整手动流程：可直接运行的单 shell 版本

公开脚本 [udp_manual_demo.sh](../../tests/udp_manual_demo.sh) 实际逐步调用 [udp_manual.py](../../tests/udp_manual.py)，不会链接测试 reactor 或注入 Options。它在同一个 namespace 中保持客户端 FIFO/socket，记录每个自有PID并等待其退出；所有输出使用新目录。以下命令是 Builder 实际执行的流程，目录和基准端口可以替换：

```bash
unshare --user --map-root-user --net sh -c 'ip link set lo up && bash tests/udp_manual_demo.sh ./build-release/bin/l4lb ./build-release/manual-evidence 18080'
```

本次只使用18080..18085：代理18080，A18081，B18082，client1/2/3为18083/4/5。若端口已被占用，先换基准端口；不要终止别人的进程。脚本正常总退出0，最后输出六个端口可重绑和完整流程PASS；所有子进程退出0，包括代理SIGINT和后端SIGTERM。以下为逐步含义和应观察的结果：

1. 启动标记A/B后端，输出 `backend ready 127.0.0.1:<端口> mark=A|B`。后端以XOR 0x55/0xaa回包，保留长度包括0；该变换属于测试工具，代理不修改数据。
2. 生成自己的UDP配置，check退出0、stderr空；启动wildcard代理，完整ready后才启动固定端口client1/2。ready不通过探测包判断。
3. client1发送带nonce的 `503100ff41` 到127.0.0.1，应匹配A和该源IP；发送 `503200ff42` 到127.0.0.2，应匹配B和该源IP。相同client socket始终为18083。
4. client1向第一目的发送零长，应收到零长；client2新key发 `7365636f6e64`，应选择A。客户端逐字节比较并打印目标、实际回复源、预期后端和长度，后端打印标记、包长及代理临时端点。
5. 只SIGTERM脚本拥有的A并wait退出0。client1向第一目的发 `6661696c6564`，有界等待应无响应；同时等待该flow id的errno111关闭日志。再显式发送新nonce `6e65772d42`，应选择B；无回复不代表TCP式EOF。
6. 在原18081恢复A；client3新key发送 `726573746f726564`，应选择A；client1继续 `737461792d42`，仍为B。恢复A不意味着已有flow自动迁移。
7. 在活跃代理占用18080时再运行同一配置，应退出1、stdout空，无ready。向三个客户端写quit并wait，SIGINT停止代理，SIGTERM停止A/B并wait，逐一重绑全部六个端口。

脚本含失败时的自有子进程清理；它不会覆盖既有证据或按进程名批量终止。故障细节查看其 `evidence=` 目录下 one/two/three、a/b、proxy、occupied 的日志。

## 单独交互操作工具

需要逐条手工观察时，在同一个正常回环环境分别打开后端、代理和客户端终端；也可先进入 `unshare --user --map-root-user --net bash` 再 `ip link set lo up`，从该shell启动子进程。不同namespace的独立终端互不可达。

```bash
python3 tests/udp_manual.py backend --bind 127.0.0.1:18081 --mark A
python3 tests/udp_manual.py backend --bind 127.0.0.1:18082 --mark B
# 代理终端：创建独立配置目录，不覆盖已有文件：
UDP_MANUAL_DIR=$(mktemp -d "$PWD/build-release/interactive-XXXXXX")
cat > "$UDP_MANUAL_DIR/udp.conf" <<'CONFIG'
protocol=udp
listen=0.0.0.0:18080
backend=127.0.0.1:18081
backend=127.0.0.1:18082
CONFIG
./build-release/bin/l4lb --run "$UDP_MANUAL_DIR/udp.conf"
python3 tests/udp_manual.py client --bind 127.0.0.1:18083
```

在客户端ready之后输入（不是重启客户端进程）：

```text
send 127.0.0.1:18080 503100ff41 A
send 127.0.0.2:18080 503200ff42 B
send 127.0.0.1:18080 - A
quit
```

语法 `send IPv4:port HEX|- A|B|none`；`-`为零长，`none`明确期待一次丢包/无响应。交互输入/系统/断言错误退出1，CLI用法错误退出2，成功quit退出0；默认收包截止1秒，不重建socket，不自动重发。零长的XOR仍为零长，单独零长不用于辨认A/B，以邻接非零nonce和后端记录作证。不要把工具的后端标记误认为生产协议字段。

## 生产构建及证据边界

```bash
cmake -S . -B build-production -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-production
./build-production/bin/l4lb --help
./build-production/bin/l4lb --check-config configs/example.conf
# 在同一正常回环环境运行快速组，测试工具使用Release构建，产品使用Production构建：
./build-release/bin/udp_product_test ./build-production/bin/l4lb ./build-release/production-evidence
./build-release/bin/udp_product_test ./build-production/bin/l4lb ./build-release/production-evidence system
```

预期全部退出0；Production不构建测试工具，也不需要Python运行服务。完整语义见 [UDP flow](../specs/udp-flow-table.md)，版本分层见 [V0.2 完成矩阵](../specs/v0.2-acceptance.md)。1024是生产默认值静态确认，容量拒绝语义由内部capacity=2动态证据验证；没有1024满载/吞吐测量。本手册快速步骤不表示已经等待60秒；只有P3验证原生产默认过期。无公网、可靠交付、源地址反欺骗或跨超时迟到包隔离承诺。

## V0.3/S1 补充

上文 V0.2 场景使用默认 health_check=off，其原11项注册保留。当前新增4项健康测试，共15项，Debug快速14/Release完整15；测试构建需 Python3 标准库，无第三方包，Production无Python依赖。`ctest --test-dir build -R v03_health` 验证默认时序真实 TCP/UDP 摘除和恢复；UDP需同IP/端口且代表UDP状态的TCP端点。既有flow不会因探活摘除而迁移/关闭，完整边界见 [健康规格](../specs/health-check.md)。

## V0.3/S2 指标验证

原场景默认metrics=off保持；新增5项指标测试，当前20项，Debug快速19/Release完整20。`ctest --test-dir build -R v03_metrics` 同时覆盖模型/实际计数落点/真实产品活跃快照。手动在配置增加metrics=stderr并把stderr重定向普通本地文件，按 [metrics规格](../specs/metrics.md) 解析前缀；同步慢sink和SIGPIPE可能影响服务，不将普通文件条件下停止界推广到堵塞管道。

## V0.3/S3 验证入口

当前新增3项系统故障场景后共23项（Debug快速22、Release完整23）。上文各阶段数量保留为历史口径；双backend故障、健康/指标联动和公开命令见 [V0.3运行手册](local-v0.3-validation.md)。
