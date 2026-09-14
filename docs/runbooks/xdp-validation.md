# V1.1 最小验证流程

本流程验证本项目XDP_PASS对象与l4lb-xdp加载工具，不验证负载均衡、map业务或性能。普通clone可使用以下命令；无需私有阶段报告、旧预检脚本或云服务器。

## 前置环境

Linux或WSL2、支持C++20的编译器、CMake≥3.20、Ninja、Python3、支持BPF的Clang、Linux UAPI开发头、libbpf≥1.0开发库。显式内核测试还需iproute2、util-linux及当前命名空间足够的BPF/网络权限。Ubuntu通常对应clang、llvm、cmake、ninja-build、python3、linux-libc-dev、libc6-dev、libbpf-dev、iproute2、util-linux；按实际缺项安装。bpftool不是前置。

本轮在Ubuntu24.04.3、WSL2内核6.6.87.2-microsoft-standard-WSL2、Clang18.1.3、libbpf1.3.0验证。其他版本需自行复验，不保证每块物理网卡native支持。先阅读[环境边界](linux-xdp-env.md)与[加载契约](xdp-loader.md)。

## 1. 默认用户态独立构建与测试

从仓库根、普通UID执行。此步骤不发现Clang BPF或libbpf，不需要root。

```bash
cmake -S . -B build-user -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-user -j4
ctest --test-dir build-user --output-on-failure
```

期望31项用户态测试通过（包含约60秒UDP expiry）。如果只需生产程序，配置时加 `-DBUILD_TESTING=OFF`，此时不需要Python且不执行CTest。

此前S2验收中宿主网络曾有4项回归失败，但独立隔离网络普通UID复验通过，宿主具体原因未确定。遇到同类端口/网络状态干扰，可采用下方隔离方式重跑，保留首次失败：

```bash
validation_user=$(id -un)
validation_build="$PWD/build-user"
sudo unshare --net bash -eu -s -- "$validation_user" "$validation_build" <<'SH'
ip link set lo up
runuser -u "$1" -- ctest --test-dir "$2" --output-on-failure
SH
```

root仅建立临时net namespace并启lo，CTest降权回原普通用户；不要直接以root执行CLI权限测试。namespace内没有宿主接口，不改变宿主路由。

## 2. 仅编译BPF对象

```bash
cmake -S . -B build-bpf -G Ninja -DL4LB_BUILD_XDP=ON
cmake --build build-bpf --target l4lb_xdp
ctest --test-dir build-bpf -L xdp_build --output-on-failure
```

期望xdp_object通过；对象为ELF64/EM_BPF/REL、仅XDP_PASS和GPL，无maps。此组合不需要libbpf开发包。工具链故障与显式构建矩阵见[BPF构建手册](xdp-build.md)。

## 3. 编译loader与无特权负向检查

```bash
cmake -S . -B build-xdp -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DL4LB_BUILD_XDP=ON -DL4LB_BUILD_XDP_LOADER=ON
cmake --build build-xdp --target l4lb-xdp -j4
build-xdp/bin/l4lb-xdp --help
ctest --test-dir build-xdp -R '^xdp_' --output-on-failure
```

期望xdp_object及xdp_loader_cli两项通过；后者内部33个用例，包含错误参数/文件/设备、map及多程序对象拒绝、普通UID权限错误。只构建loader时不要执行其他未构建的测试；运行全量CTest前先完整build。自定义libbpf目录配置见[加载手册](xdp-loader.md#构建)。

## 4. 显式真实挂载与流量验证

```bash
mkdir -p .stage-tmp/v1.1-validation
set -o pipefail
sudo python3 tests/xdp_loader_privileged.py \
  --loader "$PWD/build-xdp/bin/l4lb-xdp" \
  --object "$PWD/build-xdp/xdp/xdp_pass.bpf.o" \
  2>&1 | tee .stage-tmp/v1.1-validation/root.log
```

脚本自己创建临时net namespace及xa—xb；向xa注入原始Ethernet/IPv4/UDP帧，xb挂载程序，核对UDP接收，避免同namespace本地IP路由绕过XDP。另建xc—xd测试接口消失。generic/native-veth分别14项PASS，最后JSON含内核与loader/object/script SHA。失败返回非零，不静默跳过模式；pipefail防止tee掩盖失败。

检查包括真实ID、通流量、重复挂载拒绝、错误ID保护、INT/TERM清理、显式与重复卸载、外部替换保护、READY失败、SIGKILL恢复、关闭stdin及设备消失。默认CTest不包含此步骤。脚本清理所有子进程；namespace退出销毁其中接口，没有持久pin或宿主接口操作。

WSL若需要从Windows启动同一测试，可使用已安装发行版的官方root入口：

```powershell
wsl.exe --distribution Ubuntu --user root --exec /usr/bin/python3 /绝对仓库路径/tests/xdp_loader_privileged.py --loader /绝对构建路径/bin/l4lb-xdp --object /绝对构建路径/xdp/xdp_pass.bpf.o
```

将路径和发行版名称替换为真实值。该方式只替代sudo启动，不能跳过脚本内的隔离或真实测试；不需要把WSL默认用户改成root。

## 结果、错误与范围

[机器摘要](xdp-validation-result.json)和[六项版本验收索引](../specs/v1.1-acceptance.md)记录本轮实际结果。历史预检证据仍在[原环境摘要](local-xdp-preflight.json)，不等价当前产品测试。

权限、依赖、模式不支持、已有程序/ID不匹配等排查见[加载错误说明](xdp-loader.md#输入与错误)。遇到错误先保留完整日志和退出码，不使用force覆盖程序。V1.1实际没有maps，schema仅为[初稿](../specs/xdp-map-schema.md)；完整TCP代理、XDP负载均衡和V1.2性能对比不属于本流程。
