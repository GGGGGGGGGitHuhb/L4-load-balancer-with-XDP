# 开发与维护手册

本页说明开发者的测试筛选、生产构建与提交前格式检查。首次运行见 [README](../../README.md)，网络与故障验证见对应运行手册。命令均从仓库根目录执行。

## 测试筛选

```bash
ctest --test-dir build -N
ctest --test-dir build -LE udp_long --output-on-failure
ctest --test-dir build -R '^refactor_callback_lifecycle$' --output-on-failure
```

完整回归使用 `ctest --test-dir build --output-on-failure`；排除 `udp_long` 的快速组不能替代完整验收。测试数量以本地 `ctest -N` 为准，历史报告中的数量属于对应版本。

`s2`/`s3` 标签来自 V0.1 TCP 阶段；`udp_fast`/`udp_long` 来自 V0.2。`v03_health`、`v03_metrics` 可作为名称筛选；`v03_system` 是故障联合验证，`v04_lifecycle` 是停止与资源验证，`v04_bench_smoke` 是短 benchmark，`v04_compare` 是离线比较工具，`v10_contract` 是稳定契约验证。

正常 Linux 回环测试以普通用户运行。不可读文件用例在 root 下会跳过，root 运行不能替代普通用户权限验证。受限 WSL 环境可由必要权限创建匿名 network namespace 并启用其中的 lo，再降回普通用户运行产品与测试；不修改宿主接口、路由或 sysctl。真实内核 XDP 验证需另按 [XDP 手册](xdp-validation.md)准备隔离网络和权限。

## 生产构建

```bash
cmake -S . -B build-production -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-production -j4
```

生产构建无需 Python，不生成示例后端及测试程序。默认不构建 XDP，启用方法见 [BPF 构建](xdp-build.md)与[加载工具](xdp-loader.md)。

## Benchmark 与历史来源

当前二进制短验证与报告入口见 [README](../../README.md#benchmark当前smoke与历史报告)。完整测量、固定版本对照及离线重算分别见[用户态方法](../benchmarks/methodology.md)和 [UDP/XDP 方法](../benchmarks/v1.2-methodology.md)。

正式报告及压缩包中的源码 SHA、标签和路径描述当时被测版本；内部重构不会重写这些身份信息。离线重算使用新输出目录，不能覆盖原始数据。历史产品重建需要完整 clone 中的对应标签与提交对象。

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
