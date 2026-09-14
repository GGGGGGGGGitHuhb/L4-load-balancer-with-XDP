# V1.1/S1 可选 eBPF 构建

S1提供独立的最小 `XDP_PASS` 对象。构建不需要root、libbpf开发头、bpftool或内核BTF；不会加载或挂载程序。当前支持Linux原生构建（含WSL2），不支持CMake交叉工具链。S2独立加载器的额外选项与依赖见[加载与卸载](xdp-loader.md)。

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

产物：`build-xdp/xdp/xdp_pass.bpf.o`。只构建l4lb_xdp不会生成其他测试可执行文件；执行全量CTest前先 `cmake --build build-xdp -j4`。ON时全量build也包含BPF目标，但对象不链接进l4lb。对象检查为无特权CTest标签 `xdp_build`，仅ON且BUILD_TESTING时注册。

对象包含ELF64/EM_BPF/REL、可执行xdp section、GPL license和两条指令（r0=XDP_PASS、exit），不包含maps；测试也拒绝错误machine和返回动作的变异样本。检查是构建契约检查，不代替内核verifier或真实挂载验收。BPF始终使用-O2/-g，Debug/Release只影响用户态配置，不把-O0用于BPF。

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

[本地环境预检](linux-xdp-env.md)证明9月11日generic/native veth基础能力；S1此处只交付编译骨架，已于2026-09-14完成独立审查、Leader收尾、本地提交e01eaea及标签v1.1-s1。S2新增的项目loader、权限/接口CLI和隔离运行测试见[加载手册](xdp-loader.md)。正式map schema仍待V1.2；物理网卡native/offload和性能结论未验证，阶段标签不表示V1.1整体发布。

参考：[内核Clang说明](https://docs.kernel.org/bpf/clang-notes.html)、[CMake自定义构建规则](https://cmake.org/cmake/help/latest/command/add_custom_command.html)。

## S1 验收结果（2026-09-14）

- Builder：完整Release OFF用户态31/31（167.44秒，含udp_long）通过；Debug ON对象检查、普通UID CLI通过；OFF Debug及ON Release生产配置禁用Python发现仍成功。
- 独立Reviewer：Debug ON快速31个唯一测试通过，其中隔离网络30项（113.13秒）和普通UID CLI另1项；对象重复检查不重复计数。Release OFF构建/原31项注册、ON Release无Python对象及构建矩阵通过。
- 独立Make增量/头依赖/clean恢复、伪主机ELF编译器拒绝、格式检查通过。两项构建重编译问题（depfile空格转义、rename后的临时文件错误列为BYPRODUCT）均闭环；原失败证据保留。
- 实测CMake3.28.3/Clang18，最低CMake3.20仅核对所用接口支持，未执行该版本二进制或其他架构。功能限定原生Linux构建，不承诺交叉编译。
- 本轮用户态代码/配置保持；完整长回归由Builder执行并由Reviewer核对，未冒充独立Reviewer重复长测。完整实现指纹与日志保存于各角色证据目录，公开复核使用本页命令。
