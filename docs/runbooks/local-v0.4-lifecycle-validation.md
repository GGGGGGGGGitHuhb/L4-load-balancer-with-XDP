# V0.4/S2 本地资源与停止验证

本手册验证固定容量ring、异常安全清理和TCP有界停止，不是性能提升报告。普通Linux/WSL、C++20/Clang/CMake/Ninja及Python3标准库，无下载和外部服务；从仓库根目录运行，所有输出用新目录。

## 构建与回归

```bash
B="$PWD/.stage-tmp/lifecycle-example"
mkdir -p "$B/tmp" "$B/cache" "$B/pycache"
export TMPDIR="$B/tmp" TMP="$B/tmp" TEMP="$B/tmp" XDG_CACHE_HOME="$B/cache" PYTHONPYCACHEPREFIX="$B/pycache"
cmake -S . -B "$B/Debug" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build "$B/Debug" -j4
ctest --test-dir "$B/Debug" -LE udp_long --output-on-failure
cmake -S . -B "$B/Release" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build "$B/Release" -j4
ctest --test-dir "$B/Release" --output-on-failure
ctest --test-dir "$B/Release" -L v04_lifecycle --repeat until-fail:2 --output-on-failure
cmake -S . -B "$B/Production" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$B/Production" -j4
```

当前28项CTest，Debug排除udp_long为27项；原26项保留，新增v04_lifecycle_state、v04_shutdown_product。Release原60秒expiry执行一次，长性能矩阵不进入CTest。网络受限环境可在临时`unshare --user --map-root-user --net bash`内先`ip link set lo up`再运行网络命令；仅设置子lo，不修改宿主网络/代理/sysctl。受限执行器可能要求该边界的窄授权。

## 公开Production与工具自证

```bash
python3 tests/v04_shutdown_product.py --program "$B/Production/bin/l4lb" --output "$B/production"
python3 tests/v04_validate.py --program "$B/Production/bin/l4lb" --state-test "$B/Release/bin/v04_lifecycle_test" --output "$B/isolation-negative"
# 普通uid单独运行，包含真实不可读文件权限拒绝；namespace root不能代替
cmake -DPROGRAM="$B/Production/bin/l4lb" -DSOURCE="$PWD" -DTMP="$B/ordinary-cli" -P tests/cli_test.cmake
```

- Production脚本5场景：SIGINT空队列不等EOF、SIGTERM实际双向背压的生产1s截止、Draining屏障后的第二信号、metrics off、health+metrics下真实TCP RST。日志用普通文件。
- 业务握手确认真实backend连接，双向源socket背压和periodic发送量平台作为发送停止准备；消费信号后必须观察到结构化pending屏障且两个队列非零，不靠固定sleep猜已入队。deadline残留明确不称排空；普通日志条件下2秒预算内自行退出，不能用外层kill/timeout124当通过。
- 内部测试使用真实socketpair、小SO_SNDBUF和生产pump形成两向pending，再消费真实信号；恢复目标读后精确比对停止前已send+pending字节，证明未读后续内核数据。不把源send成功数当pending数。内部80ms截止及有限真实send进展覆盖不续期、第二信号、空OUT/poll计数、Connecting取消和token/fd释放。
- ring以固定种子deque参考逐字节对照，覆盖跨尾、重复wrap、满/空和非法提交；旧真实TCP双向8MiB+333慢读继续验证短写、背压/半关闭。
- 异常矩阵分别对TCP session/statistics/observe/DEL diagnostic及UDP flow/statistics/observe/DEL diagnostic抛出；显式close与3-owner栈展开都先释放资源/索引，独立尝试其他通知并保留原主因。DEL场景通过受控无效epoll对象触发真实EINVAL，属于故障构造，不伪称自然生产故障。
- validate脚本先两组并行，再实际空格二进制/目录；之后故意损坏期望队列、在真实后台线程抛异常、清理审计抛异常，以及内部期望字节变异/延长截止。子测试应退出1且被外层核对；每条完整argv/返回码、stdout/stderr均保留。负向不是参数错误，也不会覆盖正向证据。

每次产品run使用新UUID目录，保存config/stdout/stderr/result.json/summary.json；含真实PID/reaped、fixture fd前后、信号单调时刻/截止/双向pending、退出原因、主因/清理次因、final指标和health probe时刻。后台线程全部join，脚本只管理自身进程/socket。JSON与原始日志是证据，不从测试总耗时推算性能。失败保留原目录和错误，修复后用新目录。

## 独立Sanitizer与benchmark兼容

```bash
cmake -S . -B "$B/Sanitizer" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build "$B/Sanitizer" -j4
ctest --test-dir "$B/Sanitizer" -R 's2_net_unit|s2_tcp_integration|s2_reactor_state|v04_lifecycle_state|v02_udp_state' --output-on-failure
python3 tests/benchmark_runner.py --program "$B/Production/bin/l4lb" --protocol tcp --mode paired --duration 1 --warmup 0 --repeats 1 --output "$B/benchmark-tcp"
python3 tests/benchmark_runner.py --program "$B/Production/bin/l4lb" --protocol udp --mode paired --duration 1 --warmup 0 --repeats 1 --output "$B/benchmark-udp"
python3 tests/benchmark_acceptance.py --program "$B/Production/bin/l4lb" --suite udp-check --output "$B/udp-f001"
```

只验证当前产品与S1工具兼容，不修改统计方法或原S1样本身份，不称S3正式性能对比。TCP生产drain固定1000ms，无新CLI或配置；第二信号可合并，只有已消费到的第二次保证force。同步stderr阻塞、SIGPIPE、回调不返回及OS调度仍不属于逻辑截止的硬实时承诺。UDP保持立即停止、无排空/重传及原停止文案。详见[TCP](../specs/tcp-forwarding-semantics.md)、[UDP](../specs/udp-flow-table.md)、[metrics](../specs/metrics.md)规格。
