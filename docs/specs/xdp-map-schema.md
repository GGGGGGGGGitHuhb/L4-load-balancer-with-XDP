# XDP map schema v1

V1.2/S1，2026-09-22。共享定义为 `src/xdp/MapSchema.h`，C/BPF 和 C++ 均编译检查大小、偏移和对齐。V1.1 的 `xdp_pass.bpf.o` 仍无 maps；新增 `xdp_maps.bpf.o` 需显式 `--maps` 加载。两者都返回 XDP_PASS，S1 不解析包头、不选择后端、不转发。

## 固定布局

所有 key 为 4 字节本机序 `__u32`；cfg/stats 只用 key=0。下表 value 的数字为字节偏移。

| map 名称 | type / max_entries | value 字段 | size / align | flags |
| --- | --- | --- | --- | --- |
| `l4lb_cfg_v1` | ARRAY / 1 | `schemaVersion:__u32@0`、`backendCount:__u32@4` | 8 / 4 | `BPF_F_RDONLY_PROG` |
| `l4lb_be_v1` | ARRAY / 64 | `address:__u32@0`、`port:__u16@4`、`reserved:__u16@6` | 8 / 4 | `BPF_F_RDONLY_PROG` |
| `l4lb_stats_v1` | PERCPU_ARRAY / 1 | `passPackets:__u64@0` | 8 / 8 | 0 |

- cfg 的 schemaVersion=1，backendCount 为 0..64。后端编号为启动参数顺序，只有 `[0, backendCount)` 有效；余下槽和 reserved 全零。空集合有效，仍放行全部包。
- 地址和端口为网络字节序，其他整数为本机序。这是同机运行时 ABI，不能原样用于跨机器持久化或协议传输。
- 输入为 IPv4 字面量及 1..65535 端口；拒绝 DNS、IPv6、0.0.0.0、组播、255.255.255.255、重复 endpoint 和超过 64 项。允许 loopback/private 地址；不检查健康、路由可达性或子网定向广播。
- 没有可用状态、权重、VIP、MAC、出口接口或会话结构。S2 依据受限转发设计确定后续字段，不复用用户态 C++ 对象布局。

## 同步与所有权

控制面 `XdpConfigSync` 将完整参数转换为固定宽度数据，`MapStore` 执行内核读写。顺序为 load 未挂载对象 → 写满 64 个后端槽 → 写 cfg → 回读 cfg/全部后端 → freeze 后端及 cfg → attach → READY。只有完成回读与冻结才对包路径发布；任何一步失败非零退出，不产生 READY 或半配置挂载。

配置 maps 对 BPF 只读，freeze 后用户态也不能更新。修改配置需停止并重启，不提供热更新、健康联动、双缓冲或跨重启编号保证。此一致性来自“发布前没有读者、发布后不可变”，不宣称多次 map syscall 本身原子。

maps 由本次 object/fd 持有；程序保留三个 maps 的引用。正常条件卸载并关闭 fd 后释放，没有 pin 或旧 map 复用。SIGKILL 可能留下程序及 maps，需按实际 program ID 显式卸载；禁止写入或卸载他人资源。

## 统计口径

BPF 每次执行对本 CPU 的 passPackets 原子加一，不区分 IPv4、UDP、ARP 或其他包；cfg/backend 的 lookup 仅保持引用，尚不用于调度。用户态按 possible CPUs 和 8 字节 stride 读取汇总。单槽及汇总都是无符号 64 位模数运算。

正常 INT/TERM 后先尝试条件卸载，再输出 `XDP_STATS schema=1 pass_packets=N`。它独立于用户态 metrics schema=1。读取是非原子采样，不能承诺全 CPU 同时终值；计数不是 UDP 请求数，也不是吞吐性能。读取或输出失败返回非零，不能阻止卸载。READY 输出失败走异常清理，不保证产生统计。

## 兼容与验证

maps 模式严格校验指定程序名/section/type，以及恰好三个 maps 的 name/type/key size/value size/max entries/flags；加载后再核对内核 metadata。额外隐式全局 map、未知版本或模式错配拒绝。无 `--maps` 时保留旧单程序无 map 校验。白名单验证结构，不证明任意恶意同名 BPF 程序的语义，只加载可信构建产物。

不兼容 ABI 修改必须更换版本及白名单，不能静默重解释旧内存；没有持久化迁移承诺。布局、字节序、失败注入、实际回读/freeze、统计和资源释放验证入口见[最小验证](../runbooks/xdp-validation.md)。

关联：[加载手册](../runbooks/xdp-loader.md)、[V1.1历史验收](v1.1-acceptance.md)、[版本路线](../../ROADMAP.md)、[架构](../../ARCHITECTURE.md)。
