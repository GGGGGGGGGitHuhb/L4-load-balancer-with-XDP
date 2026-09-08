# 本地 TCP 验证

此手册只依赖仓库内源码、配置和测试，不需要本地角色报告。环境为 Linux（可用 WSL2）、C++20 编译器、CMake ≥ 3.20 和 Ninja；不下载第三方测试框架。运行需要允许本机回环 socket 和子进程。

## 构建与自动验收

在仓库根目录执行；Debug、Release 和生产构建使用不同目录，避免混用缓存。

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/tcp_product_smoke ./build/bin/l4lb ./build/bin/tcp_echo_backend ./build/product-evidence
ctest --test-dir build -L s3 --repeat until-fail:3 --output-on-failure

cmake -S . -B build-release -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
ctest --test-dir build-release -L s3 --repeat until-fail:3 --output-on-failure
```

预期：两种构建全套各 9/9（V0.2/S1 新增 `v02_scheduler_unit`，V0.2/S2 新增 `v02_udp_state` 和 `v02_udp_product`），退出 0；s3 标签恰有一个产品用例，重复 3 次均通过。直接入口最后输出中文 `PASS` 与原子唯一的 `run-XXXXXX` 目录，退出 0。该目录保留配置、各子进程 `.out`/`.err` 和 `result.txt`；后者记录 PID、动态端口、场景结果和重绑结果。测试失败退出 1，用法错误退出 2。不要把外部超时 124 当作断言成功。

若当前执行环境把 127/8 经代理接口转发，可能出现 send 成功但大 UDP 包被丢弃。可在临时用户/网络命名空间验证真实回环，不修改宿主路由：`unshare --user --map-root-user --net sh -c 'ip link set lo up && ctest --test-dir build'`。此时命名空间内 uid=0，文件读取权限用例会跳过；另在原普通用户环境运行 `ctest --test-dir build -R cli_integration` 补齐。只有在独立回环完整通过后才能将测试记为通过，不能缩小 65507 字节边界。

每次使用动态后端端口和新证据目录；代理配置仍使用正式非零端口。就绪由完整 ready 行与子进程状态确认，不用探测连接消耗轮询位置。普通 I/O 和单次就绪截止 3 秒，CTest 总截止 60 秒。只对明确的代理 bind 地址占用重试，最多 5 次。失败保留证据，测试仅停止和回收自己的子进程。

并行验证可以使用两个证据根：

```bash
./build/bin/tcp_product_smoke ./build/bin/l4lb ./build/bin/tcp_echo_backend ./build/evidence-a > ./build/a.log 2>&1 &
pid_a=$!
./build/bin/tcp_product_smoke ./build/bin/l4lb ./build/bin/tcp_echo_backend ./build/evidence-b > ./build/b.log 2>&1 &
pid_b=$!
wait "$pid_a"; result_a=$?
wait "$pid_b"; result_b=$?
printf 'A=%s B=%s\n' "$result_a" "$result_b"
```

预期 A=0 B=0。各自日志和目录隔离，历史证据不覆盖。

## 场景与诊断

- P1：实际 `l4lb --check-config`、`--run`，两个实际 echo 进程；4 个顺序连接含 NUL/非文本字节，逐字节对比，产品日志验证 A/B/A/B。
- P2：1 MiB + 17 字节（1048593），交错发送和接收，发送完 SHUT_WR，完整回包且读到 EOF。反向先 FIN、慢读背压等细项由原 S2 回归覆盖。
- P3：单后端先成功，SIGTERM 并 waitpid 回收，确认端口可绑定后发新连接并观察 EOF/reset；产品仍存活。原端口重启后端，下一连接完整成功。日志必须有连接拒绝。这里没有健康检查、自动切换、原连接重试。
- P4：坏配置与占用监听端口均让产品退出 1 且无 ready；结束后所有测试拥有的监听端口可重绑。配置只写到本轮目录，不改示例配置。

故障复现（预期工具退出 1，并清理已启动的后端）：

```bash
./build/bin/tcp_product_smoke ./build/bin/not-found ./build/bin/tcp_echo_backend ./build/failed-evidence
```

查看 stderr 指向的证据目录：`check-*.out/.err`、`echo-*.out/.err`、`proxy-*.out/.err` 和 `result.txt`。没有完整 ready、子进程提前退出或错误数据均不能产生总 PASS。端口占用先确认是自己的演示进程，勿按进程名称批量杀进程。

## 手动演示

[README 的四终端示例](../../README.md#tcp-最短运行示例)使用 8080/9001/9002，运行前保证端口空闲；需要默认 `BUILD_TESTING=ON` 提供 echo 程序。依次启动两个后端和代理，等各自 ready，再运行 Bash 客户端。预期回显 `hello S2`。各服务终端 Ctrl+C 结束；代理 SIGINT/SIGTERM 立即关闭会话，不承诺在途字节排空。

动态后端也可手动运行 `./build/bin/tcp_echo_backend 0`；读取 `echo ready 127.0.0.1:<实际端口>` 后，把该端口写入自己的配置。0 只适用于测试 fixture，产品配置端口仍为 1..65535。

## 生产独立构建

```bash
cmake -S . -B build-production -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-production
./build-production/bin/l4lb --help
./build-production/bin/l4lb --check-config configs/example.conf
```

上述命令预期全部退出 0，`build-production/bin` 只有 `l4lb`，无 echo 或测试程序。不要在已含测试产物的旧目录据此判断构建隔离。

## 验证记录与范围

2026-09-08 Builder 实跑：Linux 6.6.87.2-microsoft-standard-WSL2、Ubuntu Clang 18.1.3、CMake 3.28.3；Debug/Release 各 6/6，全套退出 0；s3 各 3 次和两实例并行退出 0；生产构建/help/check-config 退出 0。手动固定端口路径回显正确、退出后端口重绑通过。原始证据在本次工作区 `.stage-tmp/v0.1-s3/builder/`，它是可重新生成的本地记录，不是普通克隆的前置文件。

临时副本的错误数据、fixture 提前退出和缺少 ready 在 Debug/Release 共 6 次均退出 1（缺 ready 由内部 3 秒截止触发）；另外无效产品路径各退出 1、用法错误各退出 2。外部核验没有存活子进程，已记录端口均可重绑。以上为 Builder 自测；Reviewer 使用独立目录复跑全部强制项并 PASS，Leader 已完成收尾。独立证据位于 `.stage-tmp/v0.1-s3/reviewer/`，Debug/Release 各 6/6、各 s3 连续 3 次、并行、生产构建、手动路径及三类负向共 6 次均满足预期；该目录同样不作为普通克隆前提。

测试不证明公网可达性、跨主机性能或吞吐指标。上述历史记录针对 V0.1/S3 TCP 路径；当前 V0.2/S2 另提供 UDP flow 转发，见 [UDP flow 规格](../specs/udp-flow-table.md)。健康检查和 XDP 属后续版本。语义以 [TCP 规范](../specs/tcp-forwarding-semantics.md)为准。
