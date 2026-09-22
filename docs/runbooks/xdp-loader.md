# XDP 加载、map 同步与卸载

`l4lb-xdp` 是独立可选工具，可加载旧的无 map `XDP_PASS` 对象，或显式加载 V1.2/S1 带后端配置和统计 maps 的对象。默认用户态 `l4lb`、配置及 TCP/UDP 路径保持；两种 XDP 对象都放行全部包，不提供包转发或性能结论。验证入口见[最小验证流程](xdp-validation.md)。

## 构建

在[已有BPF工具链](xdp-build.md)基础上，需要libbpf >= 1.0开发头和可链接库。Ubuntu可执行 `sudo apt install libbpf-dev`；不需要pkg-config或bpftool。不要使用sudo构建。

```bash
cmake -S . -B build-xdp -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DL4LB_BUILD_XDP=ON -DL4LB_BUILD_XDP_LOADER=ON
cmake --build build-xdp -j4
build-xdp/bin/l4lb-xdp --help
ctest --test-dir build-xdp -R '^xdp_' --output-on-failure
```

两个选项都默认OFF。只开 `L4LB_BUILD_XDP` 仍只构建BPF对象，不依赖libbpf。开启loader而关闭BPF构建会明确失败。依赖在自定义目录时，可设置 `L4LB_LIBBPF_INCLUDE_DIR`（包含bpf子目录）和 `L4LB_LIBBPF_LIBRARY`（共享库文件路径）；若使用静态库还须提供其传递依赖，建议使用发行版共享开发库。`BUILD_TESTING=OFF`不需要Python。

## 先在隔离网络验证

以下 sudo 命令运行仓库旧模式显式测试。脚本先创建独立 network namespace，只在其中创建 veth，退出时销毁临时网络，不修改宿主网卡。需要 Python3、iproute2、util-linux 和允许加载 BPF 的 root 环境。新增 maps 的真实测试命令见[最小验证流程](xdp-validation.md#5-map-同步与故障验证)。

```bash
sudo python3 tests/xdp_loader_privileged.py \
  --loader "$PWD/build-xdp/bin/l4lb-xdp" \
  --object "$PWD/build-xdp/xdp/xdp_pass.bpf.o"
```

测试分别验证generic与native-veth，实际挂载并查询程序ID，发送Ethernet/IPv4/UDP帧验证PASS，检查重复挂载、错误ID、显式卸载、重复卸载、INT/TERM清理、外部替换保护、READY输出失败、SIGKILL后恢复、标准输入关闭和设备消失。任一失败返回非零，不会跳过native后声称通过。它不属于默认CTest；普通UID的 `xdp_loader_cli` 不应在root/user-namespace-root中运行。

## 手动操作

仅在明确选择的测试设备上运行。以下以已经创建且UP的 `xb` 为例；命令不会自动创建接口。

```bash
sudo build-xdp/bin/l4lb-xdp attach --dev xb \
  --object build-xdp/xdp/xdp_pass.bpf.o --mode generic
```

成功后输出 `READY dev=xb mode=generic prog_id=...` 并等待。按Ctrl+C或发送SIGTERM会条件卸载，输出DETACHED后退出。默认mode为generic；native必须显式选择，不自动回退。工具拒绝覆盖已有XDP程序。

需要另一个终端显式卸载时，使用READY或 `ip -details link show dev xb` 得到的实际ID：

```bash
# 将123替换为刚刚确认的实际prog_id
sudo build-xdp/bin/l4lb-xdp detach --dev xb --mode generic --prog-id 123
```

正常卸载或该模式已无程序返回0；不同ID/竞态失败返回1，保留其他程序。显式卸载后前台进程仍等待，可用Ctrl+C结束；若中途挂了新程序，旧进程退出不会卸载它，并以1报告ID不匹配。SIGKILL、进程崩溃或主机异常不能保证自动清理，应使用已确认ID显式卸载；不提供无条件force卸载。

## V1.2/S1 后端同步与统计

在已创建的测试设备上使用新增对象；`--backend` 可重复，`--maps` 只能出现一次，顺序不限：

```bash
sudo build-xdp/bin/l4lb-xdp attach --dev xb \
  --object build-xdp/xdp/xdp_maps.bpf.o --maps \
  --backend 10.0.0.1:8000 --backend 10.0.0.2:8001 --mode generic
```

不传后端表示空集合。最多 64 个唯一 IPv4:PORT；输入限制和布局见[map schema](../specs/xdp-map-schema.md)。只在启动时写入和回读完整配置，再冻结并挂载；成功 READY 追加 `schema=1 backend_count=2`。更新后端需停止重启，没有健康检查联动或运行期写入。

正常停止先尝试卸载，再输出 `XDP_STATS schema=1 pass_packets=N`。N 是所有 CPU 的 XDP_PASS 包计数采样，包含背景包，不等同业务请求数；无包头解析或后端转发。统计或输出失败返回 1，仍执行卸载。SIGKILL 恢复沿用按 ID 显式卸载，同时释放程序引用的 maps。

写入、回读或 freeze 失败不挂载、不输出 READY，关闭本次对象；freeze 不支持不会降级绕过。错误包含操作上下文，系统调用错误附 errno。内核 map 没有 pin，不复用他人 map。

## 输入与错误

- `--help`独立使用返回0；语法错误返回2；运行或输出失败返回1。除 attach 的 `--maps` 模式允许重复 `--backend` 外，选项不允许重复、未知或跨子命令混用。设备名为1—15字节，不包含空白、斜杠或冒号；ID为正uint32十进制数。
- 对象必须为非空普通文件，最多16 MiB；用同一次打开读取的有界字节解析加载，避免检查后路径被替换。旧模式拒绝所有 maps；maps 模式只接受指定程序及三个 maps 的完整元数据，不兼容版本、缺失/额外 map、额外程序、错误 section/type/name 均拒绝。形状检查不是安全审计：只加载可信的项目对象，不能保证任意同名程序语义为 PASS。
- 缺开发依赖：安装libbpf-dev或修正两个CMake依赖路径；若仅编译BPF，关闭loader选项。
- 找不到设备：在同一network namespace内用 `ip link` 核对设备名。
- EPERM/EACCES：检查sudo、CAP_BPF/CAP_NET_ADMIN、容器限制和memlock；root不等于拥有全部能力。libbpf可能先输出低层加载探针错误，随后本工具补充操作上下文。工具不自动修改系统限制。
- native不支持：核对内核与驱动；可自行显式改generic。WSL2 native-veth通过不表示无线网卡或物理NIC支持。
- 已挂载/ID不匹配：先核对实际程序及其拥有者，不能用猜测ID或无条件卸载强行覆盖。

当前功能验证路线见[Linux/WSL2环境](linux-xdp-env.md)。结果仅覆盖记录的本地内核和虚拟接口，不推导物理NIC/native/offload或性能结论。

## S2 验收结果（2026-09-14）

独立Reviewer PASS，S2 Completed。Ubuntu24.04.3/WSL2内核6.6.87.2-microsoft-standard-WSL2、Clang18、libbpf1.3.0：Builder Release与Reviewer独立Debug产物均完成generic/native-veth各14项真实验证。关闭stdin时libbpf把FD0视为未指定比较的边界已修复并复验；比较FD统一复制到>=3。

普通UID CLI脚本33用例通过。当前ON两选项的CTest共33项均有独立PASS证据：宿主环境29/33，四项网络测试失败后在隔离namespace普通UID中4/4通过；未确定宿主失败具体原因，不宣称宿主整组通过。S1构建矩阵仍通过，默认OFF/S1-only不受loader依赖影响。以上为S2验收时的记录；S3最新验证见[版本验收索引](../specs/v1.1-acceptance.md)。其他内核/物理网卡和性能未验。
