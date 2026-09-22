# 可选 eBPF 对象构建

提供独立的最小 `XDP_PASS` 对象、V1.2/S1配置/统计maps PASS对象，以及V1.2/S2的UDP二层DSR对象。构建不需要 root、libbpf 开发头、bpftool 或内核 BTF；不会加载或挂载程序。当前支持 Linux 原生构建（含 WSL2），不支持 CMake 交叉工具链。独立加载器的额外选项与依赖见[加载与卸载](xdp-loader.md)。

## 默认用户态构建

`L4LB_BUILD_XDP` 默认OFF，原[README构建命令](../../README.md)保持。不检测BPF Clang和头文件，不生成BPF对象。`BUILD_TESTING=OFF`不需要Python；原有测试仍使用Python3。

## 开启 BPF 构建

需要支持BPF目标的Clang和Linux UAPI开发头文件；Ubuntu本轮验证使用Clang18及linux-libc-dev。BPF是受限C，和用户态C++编译器分别选择。

```bash
cmake -S . -B build-xdp -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DL4LB_BUILD_XDP=ON
cmake --build build-xdp --target l4lb_xdp
ctest --test-dir build-xdp -L xdp_build --output-on-failure
```

产物：`build-xdp/xdp/xdp_pass.bpf.o`、`build-xdp/xdp/xdp_maps.bpf.o` 和 `build-xdp/xdp/xdp_udp_dsr.bpf.o`。只构建 l4lb_xdp 不会生成其他测试可执行文件；执行全量 CTest 前先 `cmake --build build-xdp -j4`。ON 时全量 build 也包含 BPF 目标，但对象不链接进 l4lb。旧对象检查为无特权 CTest 标签 `xdp_build`，仅 ON 且 BUILD_TESTING 时注册；maps/DSR对象的ABI及同步检查随可选loader测试运行，真实内核与DSR往返检查见[验证流程](xdp-validation.md)。

旧对象包含 ELF64/EM_BPF/REL、可执行 xdp section、GPL license 和两条指令（r0=XDP_PASS、exit），不包含 maps；旧测试仍拒绝错误 machine 和返回动作的变异样本。新对象使用本地 BTF map 声明和 Linux UAPI，共用 `MapSchema.h`，含三个 maps、lookup 与 per-CPU 原子计数，仍返回 PASS。检查不代替内核 verifier 或真实挂载验收。BPF 始终使用 -O2/-g，Debug/Release 只影响用户态配置，不把 -O0 用于 BPF。

DSR对象另用 `UdpDsrSchema.h`，包含v2配置、目标和统计map；有界解析IPv4/UDP、按五元组选目标、仅改MAC后redirect。旧PASS对象指令检查不适用于DSR；必须通过独立verifier/test-run和实际后端收发验收，不以ELF存在就证明转发成功。

## 编译器和头文件

- `-DL4LB_BPF_CLANG=/usr/bin/clang-18`：显式指定绝对可执行路径；默认寻找clang及部分带版本后缀候选。若发现的Clang不支持BPF会报错，应明确选择正确的Clang。
- `-DL4LB_BPF_INCLUDE_DIRS='/path/to/include;/path/to/multiarch'`：补充系统头目录。默认按C++工具链的CMAKE_LIBRARY_ARCHITECTURE加入存在的 `/usr/include/<multiarch>`，本次为x86_64-linux-gnu；不会把该架构硬编码为所有平台。
- 开启时配置阶段真实编译探针，缺编译器、BPF后端、linux/bpf.h或asm/types.h会非零失败并给出诊断；不自动下载依赖、不静默回退OFF。
- 生产仅编译对象可加 `-DBUILD_TESTING=OFF`，此时不需要Python或LLVM查看工具。

## 重建、清理与异常测试

Ninja和Unix Makefiles支持源码/头文件depfile增量重建；没有改动时不重编译。失败重建移除旧的正式对象，成功编译临时文件后再发布，避免旧对象被误认为本次成功结果。clean清理对象、临时对象和depfile。关闭选项不会删除旧build目录遗留对象，应使用新目录区分构建配置。

可选手工检查（需要相应LLVM工具）：

```bash
llvm-readelf-18 -h build-xdp/xdp/xdp_pass.bpf.o
llvm-objdump-18 -d build-xdp/xdp/xdp_pass.bpf.o
```

显式构建矩阵不进入默认CTest，约几十秒；需要cmake、Ninja、make、Clang、Python3，从仓库根运行。work必须是尚不存在的新目录；脚本只修改该目录内的源码副本。

```bash
python3 tests/xdp_build_test.py --work .stage-tmp/xdp-build-check
```

覆盖OFF无效BPF依赖、ON缺编译器/失败编译器/坏头文件、空格目录、无改动build、源/头变更、失败重建不遗留正式对象、恢复、clean及Make构建。日志保存在work，各负向是注入场景，不代表主机缺依赖。

## 当前验证边界

[本地环境预检](linux-xdp-env.md)证明9月11日 generic/native veth 基础能力；V1.1/S1 编译骨架于2026-09-14完成独立审查、Leader收尾、本地提交e01eaea及标签v1.1-s1。后续 loader 与 V1.2/S1 maps 使用方式见[加载手册](xdp-loader.md)，正式布局见[map schema](../specs/xdp-map-schema.md)。物理网卡 native/offload 和性能结论未验证，阶段标签不等于整个版本发布。

参考：[内核Clang说明](https://docs.kernel.org/bpf/clang-notes.html)、[CMake自定义构建规则](https://cmake.org/cmake/help/latest/command/add_custom_command.html)。

## S1 验收结果（2026-09-14）

- Builder：完整Release OFF用户态31/31（167.44秒，含udp_long）通过；Debug ON对象检查、普通UID CLI通过；OFF Debug及ON Release生产配置禁用Python发现仍成功。
- 独立Reviewer：Debug ON快速31个唯一测试通过，其中隔离网络30项（113.13秒）和普通UID CLI另1项；对象重复检查不重复计数。Release OFF构建/原31项注册、ON Release无Python对象及构建矩阵通过。
- 独立Make增量/头依赖/clean恢复、伪主机ELF编译器拒绝、格式检查通过。两项构建重编译问题（depfile空格转义、rename后的临时文件错误列为BYPRODUCT）均闭环；原失败证据保留。
- 实测CMake3.28.3/Clang18，最低CMake3.20仅核对所用接口支持，未执行该版本二进制或其他架构。功能限定原生Linux构建，不承诺交叉编译。
- 本轮用户态代码/配置保持；完整长回归由Builder执行并由Reviewer核对，未冒充独立Reviewer重复长测。完整实现指纹与日志保存于各角色证据目录，公开复核使用本页命令。
