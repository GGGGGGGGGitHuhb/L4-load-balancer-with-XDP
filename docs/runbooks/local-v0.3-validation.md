# V0.3 本机故障场景验证

适用 V0.3-S3-D1。普通 clone 使用以下已跟踪工具即可重现，不需要角色临时目录。需要 Linux、Clang/C++20、CMake、Ninja、Python3 标准库；隔离路线另需 util-linux 的 unshare 和 iproute2。生产构建无 Python 依赖。

## 构建与运行

从仓库根目录执行。stderr 写普通文件；每次运行自动创建唯一 run-* 子目录，支持空格路径及并行独立证据根。

```bash
mkdir -p validation-v03/tmp validation-v03/cache validation-v03/pycache
export TMPDIR="$PWD/validation-v03/tmp" TMP="$PWD/validation-v03/tmp" TEMP="$PWD/validation-v03/tmp"
export XDG_CACHE_HOME="$PWD/validation-v03/cache" PYTHONPYCACHEPREFIX="$PWD/validation-v03/pycache"
cmake -S . -B build-release -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j4
unshare --user --map-root-user --net bash -c 'ip link set lo up && bash tests/v03_validate.sh build-release/bin/l4lb "validation-v03/product evidence"'
```

只启用子 namespace 的 lo，不修改宿主网络、代理或 sysctl。WSL 宿主 loopback 曾存在大 UDP 包限制，完整 UDP 回归使用此路线。系统不允许非特权 user namespace 时，需要执行环境允许此隔离操作；不要改小原 65507 字节断言。

脚本依次执行 tcp、udp、failure，任一步失败即非零退出。也可单独执行：

```bash
python3 tests/v03_system_test.py build-release/bin/l4lb validation-v03/one tcp
python3 tests/v03_tool_negative.py build-release/bin/l4lb validation-v03/tool-negative
```

上面网络命令需在已启用 lo 的独立 namespace 中执行。正式注册名为 v03_system_tcp、v03_system_udp、v03_system_failure，标签 v03_system，各90秒上限，实际单次双backend场景约19秒；状态屏障使用生产固定1秒周期与2成功/3失败阈值，等待上限只作失败界。

```bash
# 在独立 namespace 内执行以下网络测试。
ctest --test-dir build-release --output-on-failure
ctest --test-dir build-release -L v03_system --repeat until-fail:2
# Debug 使用同样配置过程但改为 Debug、build-debug。
ctest --test-dir build-debug -LE udp_long --output-on-failure
```

当前31项：Debug快速30，Release完整31（V0.3/S3历史为23/22快速）含一次原60秒 expiry；重复组只有新增3项，不含 expiry。Production 使用 -DBUILD_TESTING=OFF 构建后，可运行同一 v03_validate.sh。普通 uid 另运行 `cmake -DPROGRAM="$PWD/build-release/bin/l4lb" -DSOURCE="$PWD" -DTMP="$PWD/validation-v03/cli" -P tests/cli_test.cmake`，当前44项包含不可读文件（V0.3/S3历史38项）；namespace root 会跳过权限项，不冒充普通用户验证。

## 场景与配置

fixture 动态分配 loopback 端口并保存实际 config.conf。P1/P2 的配置结构如下（数字端口仅说明结构，脚本会分配可用端口）：

```ini
protocol=tcp
listen=127.0.0.1:18080
backend=127.0.0.1:19001
backend=127.0.0.1:19002
health_check=tcp_connect
metrics=stderr
```

P2 改 protocol=udp；每个 backend 的 TCP 健康监听与 UDP 数据服务使用同 IP/数字端口。P3 使用 protocol=tcp、health_check=off，按 dead/live 顺序配置。两开关默认均 off；ready 不代表 Healthy。`--check-config` 只检查配置，不建网络连接、不输出指标。

- P1：先建立旧 A/B 会话；仅关闭 A 监听，旧连接保持且4个新连接只到 B；再关 B，2个新连接被拒且 created 不增，旧连接仍能传数据；只恢复 A 验证2个新连接，然后恢复 B，按当前轮询 cursor 验证连续4个后端身份。
- P2：只停 TCP 健康监听，UDP 服务一直工作；旧 flow 的源端点绑定保持，新 key 按资格选择。全不可选的2包 rejected/dropped 各+2；串行零长包 datagrams+1、bytes+0。分步恢复与当前 cursor 验证同 P1。
- P3：health off，真实 dead 端口两次 ECONNREFUSED，中间独立新会话到 live；失败 nonce 不达 live、无自动重试，errors=2，失败生命周期计入 created/closed，成功 bytes 只来自 live。

## 判定与失败证据

每个 run-* 保存 config.conf、产品 stdout/stderr、完整 snapshots.json、backend-events.json、actions.json、ledger.json、cleanup.json 和 result.json。实际文件名以目录内容为准；动作使用单调时间，nonce 与 backend 身份独立于产品指标，TCP fixture 的4字节长度帧也计入实际转发 bytes；UDP 零包独立串行观测。健康 TCP 空连接单独记作 probe，不计业务。

解析仅接收完整 `metrics ` 前缀 JSON 行，沿用 [指标规格](../specs/metrics.md) 的 schema、连续 seq 和时序规则。各阶段预期计数从已确认 nonce 账本计算，再与实际快照比较；最终仅一个 final、active=0，关闭计数完整。旧 flow 没有迁移不等于不健康后端立刻不再接收旧业务。

脚本只清理自己拥有的 PID/socket/thread，普通文件 stderr 下按原2秒停止界等待，必要时 kill 并回收；原业务失败与次生 cleanup 错误分开保存。cleanup.json 记录 PID 消失、fd 前后数量、线程关闭结果。工具负向使用错误 backend、退出17、损坏 seq、线程异常，以及主因叠加清理错误；每个子用例应退出1，外层确认目标主因与清理后才返回0，不以 timeout124当成功。

同步 stderr 可能阻塞业务/停止，关闭管道可能 SIGPIPE；本手册不保证慢/堵塞 sink 的退出时间，不保证应用消费成功，不提供性能或稳定性概率结论。

本阶段 Builder 已实际执行上述跟踪 runner 的 Release 普通/空格路径、并行两组及 Production 三场景。ICMP Debug/Release 各5次新 namespace 的采样与完整回归分别留档；历史偶发根因未定位，不宣称修复。最终证据及审查状态见 [版本验收矩阵](../specs/v0.3-acceptance.md)。
