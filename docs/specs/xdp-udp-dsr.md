# XDP UDP 二层 DSR

V1.2/S2，2026-09-22；本页记录已实现并独立验收的契约。真实测试与源码/产物身份见[验证摘要](../runbooks/xdp-dsr-validation-result.json)，复现见[验证手册](../runbooks/xdp-validation.md)。

## 能力与部署前提

`xdp_udp_dsr.bpf.o` 是独立的 IPv4/UDP 二层转发对象，使用 `--udp-dsr` 显式启用。单个 VIP/端口，最多64个静态目标；每个目标是出口接口与后端 MAC。按五元组稳定选择目标，只改 Ethernet 源/目的 MAC，IP地址、端口、TTL、IP/UDP校验和、payload与尾部padding保持原样。

后端须在自身配置相同 VIP/32，UDP服务绑定该VIP及同一端口，并具备直接到客户端的回程。产品不配置地址、路由、邻居或接口。该方式不建立用户态代理socket、不实施NAT或会话翻译，不是现有TCP/UDP代理的透明替换。

入口和出口均须是同一netns中已存在、UP、MAC有效、MTU至少1500的Ethernet接口；出口不能是入口。目标MAC严格为六组两位十六进制，拒绝全零、广播和组播；重复ifindex+MAC组合拒绝。零目标有效，匹配服务的包仍PASS。配置修改需停止重启；S3再实现运行期更新与健康联动，S4再作性能比较。

## 命令与兼容性

以下需先准备好测试设备与后端地址；程序不会创建拓扑：

```bash
sudo build-xdp/bin/l4lb-xdp attach --dev ingress0 \
  --object build-xdp/xdp/xdp_udp_dsr.bpf.o --udp-dsr \
  --vip 198.18.100.100:39001 \
  --target 'egress0@02:00:00:00:00:11' \
  --target 'egress1@02:00:00:00:00:12' --mode generic
```

`--udp-dsr` 与 `--maps` 互斥；`--target` 可重复，`--vip` 只能一个；原 `--backend` 仅适用于S1 maps模式。detach不接收这三类配置参数。VIP为严格IPv4字面量与1..65535端口，不接受DNS、IPv6、0.0.0.0、组播、全1广播。默认generic，native须显式指定；不静默降级。

旧无map对象与S1 schema=1对象/CLI保持。新对象严格校验单程序name/section/type及恰好三个v2 maps的全部metadata，不接受profile错配、额外map（含隐式全局变量）、额外程序或未知ABI。只加载可信项目对象，白名单不是恶意BPF程序的语义证明。

## 启动与停止

完整参数/接口校验 → 打开并检查对象 → load创建未挂载maps → 写满64个backend槽和cfg → 全量回读 → freeze配置maps → attach → READY。失败不发布半份配置、不输出READY；对象和fd随RAII清理。只读接口状态，不自动修复网络环境。

READY带 `schema=2 profile=udp-dsr backend_count=N` 及已有dev/mode/prog_id字段。INT/TERM停止先按预期ID/FD条件卸载，再汇总统计，输出 `XDP_STATS schema=2`；统计或输出错误不能阻止卸载。保留退出码0成功、2参数错误、1运行/输出失败。

SIGKILL可能留下附着程序与其maps，应确认当前实际program ID后使用原detach命令恢复。禁止force覆盖、无条件卸载、pin或任意ID写入他人maps。出口在运行中down/消失可能丢包；需操作者恢复配置并重启，不提供健康摘除或自动换后端。

## 逐包判定与流映射

- 支持完整Ethernet IPv4/UDP包：version=4、IHL=5、IP总长28..1500且在data_end内，无分片（允许DF，拒绝MF/offset及保留标志），IPv4头校验和正确；UDP length等于IP总长减20。允许Ethernet尾部padding。
- VLAN、IPv6、TCP、options、分片、越界/长度异常、坏IPv4校验和、非VIP或非服务端口：原帧PASS。PASS仅交还当前主机网络栈，不承诺进入用户态l4lb代理，也不保证栈会接收无效报文。
- 配置缺失/版本错误/count越界/reserved非零，或所选目标缺失/无效：原帧PASS并记invalidConfig。count=0记noBackend。stats无法取得时原帧PASS，无法保证产生计数。
- 五元组hash使用32位FNV-1a：初值2166136261，依次处理网络序srcIPv4四字节、dstIPv4四字节、srcPort两字节、dstPort两字节、protocol一字节；每字节先xor再乘16777619，按32位截断。目标索引为hash%count，同流/同配置稳定；没有流表，不承诺配置变化后的流粘性或无损迁移。
- 正常命中调用 `bpf_redirect(ifindex, 0)`，其返回XDP_REDIRECT后写入出口源MAC和后端目的MAC，并返回REDIRECT。helper即时失败返回DROP并记helperError；不修改包，也不重试其他目标。
- 不验证或重算UDP校验和；包括零校验和在内原值保持，由后端UDP栈负责有效性检查。没有TTL递减，因为这是受限二层转发，不是IP路由器。

## ABI v2与统计

共享头 `src/xdp/UdpDsrSchema.h` 同时供C/BPF与C++编译，固定宽度数据、显式size/alignment/offset断言。所有key为4字节本机序u32，cfg/stats仅用key=0；IP/端口为网络序、MAC为6字节数组，其余整数本机序。

| map | type / capacity / flags | value布局 |
| --- | --- | --- |
| `l4lb_cfg_v2` | ARRAY / 1 / BPF_F_RDONLY_PROG | schemaVersion u32@0、backendCount u32@4、vipAddress u32@8、vipPort u16@12、reserved u16@14；size16 align4 |
| `l4lb_be_v2` | ARRAY / 64 / BPF_F_RDONLY_PROG | ifindex u32@0、destinationMac u8[6]@4、sourceMac u8[6]@10；size16 align4 |
| `l4lb_stats_v2` | PERCPU_ARRAY / 1 / 0 | 以下8个u64按表顺序排列；size64 align8 |

cfg版本为2，count范围0..64，reserved和未用backend槽为零；配置map挂载前冻结，BPF只读，运行期用户态也不能修改。stats每CPU原子更新，用户态按possible CPUs和64字节stride汇总；计数按u64模数运算，快照不保证所有CPU同一时刻。

| 字段 / 偏移 | 意义 |
| --- | --- |
| totalPackets / 0 | 取得stats后观察到的包 |
| passPackets / 8 | 返回PASS |
| redirectRequests / 16 | 返回REDIRECT的请求数，不是后端送达数 |
| dropPackets / 24 | helper即时错误导致的DROP |
| unsupportedPackets / 32 | 不支持、不匹配或格式不合格导致的PASS |
| noBackendPackets / 40 | 匹配服务但零目标导致的PASS |
| invalidConfigPackets / 48 | 配置/目标无效导致的PASS |
| helperErrorPackets / 56 | redirect helper即时失败 |

实际发送发生在BPF程序返回后；出口失效、驱动限制等异步失败不一定能从程序计数得知。因此用接收端证明送达，不能用redirectRequests冒充吞吐或成功响应。[内核redirect说明](https://docs.kernel.org/bpf/redirect.html)解释了helper与后续发送的阶段边界。

## 验证与限制

真实内核test-run验证边界、action、改包字节与统计；实际隔离客户端/LB/两个后端拓扑验证generic/native-veth下请求及直接回包，同流同后端、不同向量覆盖两个后端，并验证未命中包交回主机栈。错误路径覆盖同步/冻结、生命周期、入口/出口消失及统计/输出失败。入口见[XDP验证流程](../runbooks/xdp-validation.md)。

默认用户态和旧两种XDP模式需要回归；测试不依赖云服务器，不修改宿主接口。结果只适用于记录的Linux/WSL2虚拟网卡环境，不推出物理NIC/native/offload或生产性能结论。不提供TCP、IPv6、NAT、动态邻居、复杂conntrack、热更新或健康联动；后两项已列入S3。
