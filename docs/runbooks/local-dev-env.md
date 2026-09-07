# 本机开发环境与启动验证

验证日期：2026-09-07。以下探测记录适用当时的 V0.1/S1 用户态开发准备，不代替项目验收；当前 S1 已完成，真实项目证据见末段。

## 已验证环境

- WSL2：Linux 6.6.87.2-microsoft-standard-WSL2。
- Clang++：18.1.3；CMake：3.28.3；Ninja：1.11.1。
- CTest、clang-format 已安装。
- 临时 C++20 工程：CMake 配置、Ninja 编译、CTest 运行通过；使用 `std::span` 验证 C++20 编译和运行。
- epoll 创建与关闭通过。
- 本地 TCP 随机回环端口 bind/listen/connect、请求发送及响应返回通过。只连接本机，socket 随上下文退出关闭。

## 复现与解释

工具版本使用 `clang++ --version`、`cmake --version`、`ninja --version` 检查。

TCP 检查可在普通 WSL 终端运行：

```bash
python3 - <<'PY'
import socket
with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    with socket.create_connection(listener.getsockname(), timeout=2) as client:
        peer, _ = listener.accept()
        with peer:
            peer.settimeout(2)
            client.sendall(b'ready')
            assert peer.recv(5) == b'ready'
            peer.sendall(b'OK')
            assert client.recv(2) == b'OK'
print('Local TCP: PASS')
PY
```

启动准备时 Agent 受限环境直接创建 TCP socket 返回 `EPERM`；在允许本机网络的执行上下文复测通过。因此后续网络集成测试应在允许 socket 的 WSL 执行环境运行，不需要因此修改系统网络配置或使用 root。S1 的配置测试本身不需要创建网络 socket。

首次组合探测中，编译成功但包含 socket 的 CTest 失败；拆分后纯 C++20 的 CTest 通过、epoll 通过，网络探测在上述允许环境中通过。该失败未被计为项目测试失败或伪记为通过。

准备探测当时尚无项目 CMake、源码和测试。随后 S1 已落地；Builder 报告 `docs/builder/reports/V0.1/S1-report-001.md` 和 Reviewer 报告 `docs/reviewer/reports/V0.1/S1-report-001.md` 记录真实工程 Debug/Release 构建与测试通过，最终收尾见 Leader 报告 003。云服务器、libbpf 和 XDP attach 测试属于后续版本，本次未验证。
