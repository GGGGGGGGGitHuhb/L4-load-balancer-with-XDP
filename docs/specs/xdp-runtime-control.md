# XDP 运行期配置与 UDP 健康联动

V1.2/S3，已完成独立验收与收尾（2026-09-28），尚未提交或发布；本页描述新profile的实现契约，阶段状态以README/ROADMAP及最终验证摘要为准。旧[静态DSR](xdp-udp-dsr.md)保持独立，不解除schema v2冻结。

## 启动和重载

新对象`xdp_udp_runtime.bpf.o`通过独立参数启用：

```bash
sudo build-xdp/bin/l4lb-xdp attach --dev ingress0 \
  --object build-xdp/xdp/xdp_udp_runtime.bpf.o --udp-dsr-runtime \
  --vip 198.18.0.100:9000 --runtime-config /absolute/path/dsr-runtime.conf \
  --mode generic
```

必须先按[DSR部署前提](xdp-udp-dsr.md#能力与部署前提)配置后端VIP、服务和直接回程；程序不更改接口、地址、路由、邻居。runtime还要求LB能通过每个出口到达对应UDP探测端点，并能收到其回复。

`--udp-dsr-runtime`与`--maps`、`--udp-dsr`互斥，禁止`--backend`和`--target`；`--runtime-config`只适用于新profile的attach。VIP、入口和mode启动后固定，改变需停止重启。detach继续使用原有`--dev --prog-id --mode`，不新增profile标志。

程序保存配置路径的绝对形式；操作者在同目录写新文件后rename替换，再向loader进程发送SIGHUP：

```bash
kill -HUP LOADER_PID
```

HUP合并为一个待处理请求，不自动监视文件。坏文件、坏接口、资源或提交前发布故障保持旧配置和探测集合；修复后重新HUP。相同规范化配置不发布新代次，不重置健康。配置解析与接口校验仍会执行。

## 配置文件

```text
schema=1
target=blue out0@02:00:00:00:00:02 10.20.0.2:9001
target=green out1@02:00:00:00:00:03 10.30.0.2:9001
```

- 必须恰好一条`schema=1`，0..64条target。target三个字段依次为`ID`、`EGRESS@MAC`、`PROBE_IP:PORT`，使用空格/tab分隔。
- ID为1..32字符`[A-Za-z0-9_-]`。拒绝重复ID、重复ifindex+MAC、同出口重复probe端点。IP、端口、MAC和接口要求沿用S2；不支持DNS或IPv6。
- 最多64KiB、256行、每行1024字节；支持LF/CRLF、空行、首个非空字符为#的整行注释。拒绝NUL、非ASCII、裸CR、额外token和未知字段，不做include/变量/命令展开。
- 使用同一次open的有界普通文件，拒绝最终路径symlink、目录、FIFO等；读取前后核对文件属性。受支持编辑方式是原子替换，不保证检测所有并发原地修改；产品不写配置。
- 全部身份字段（ID、ifindex、源/目的MAC、probe地址/端口）相同的目标保留已完成健康状态；重排保持状态。变更身份的新目标Unknown；删除目标取消探测。实际接纳新配置时在途探测全部取消，新探测使用新nonce。

## 探测、摘除与恢复

新profile强制UDP echo健康检查。每目标须提供可达的echo端点，完整返回请求；可用业务端口（仅业务协议本身兼容echo时），也可单独端口。probe地址必须为后端独立地址，任何端口都不得使用共享VIP，否则配置被拒绝。独立echo端点应由部署方关联业务健康；其存活不证明任意业务逻辑健康。

请求24字节：`L4LBHC01`八字节ASCII + 进程随机epoch的网络序u64 + 全进程单调sequence的网络序u64。只有connected UDP peer完整回显原24字节，且在deadline之前，才成功。错/短/长/重复/晚回复和其他peer都不能成功；UDP connect成功或TCP可连均不能代替应答。nonce用于身份匹配，不是敌对网络认证。

- 固定间隔1秒、超时500毫秒，steady_clock，不追赶积压周期；每目标最多一个在途探测，最多64个。socket为NONBLOCK/CLOEXEC并绑定出口。
- 初始Unknown不可选；连续2次成功进入Healthy，连续3次失败进入Unhealthy；成功/失败互相清零，计数饱和。健康转换仅生成期望集合，发布成功后才影响数据面。
- deadline相等也算超时；处理回复完成时再次核对时间。每轮最多256个接收事件，控制循环约20ms唤醒收割，避免等到deadline才读取已有回复。
- 拒绝、不可达、静默或目标相关错误计失败；本机资源故障标记local_error并保守计失败，不宣称确定后端本体故障。内部时钟/身份/轮询错误终止清理，不能停止探测却永久保留Healthy。
- 健康发布失败继续使用旧快照，至多每秒一次重试；失败期间坏后端可能仍在已应用集合。状态日志与已应用generation分开说明。

## 数据面与流语义

仅支持静态DSR相同的有界Ethernet/IPv4/UDP解析、IP checksum、长度、分片规则和FNV-1a五元组映射。仍只改两MAC，IP、端口、TTL、UDP checksum、payload及padding保持；后端直接以VIP回包。

动态profile对**匹配服务且有效的包，在可选目标为0时DROP**，增加dropPackets和noBackendPackets。启动Unknown、配置空集合及全后端不健康均适用。旧v2零目标PASS保持。非目标/不支持/坏配置的PASS、helper即时失败DROP和异步redirect不等于实际送达的边界仍见[DSR逐包规则](xdp-udp-dsr.md#逐包判定与流映射)。

健康目标按文件顺序构成致密数组。重载或健康变化可能改变目标数量/顺序，已有UDP五元组可能重新映射；没有流表、连接draining、失败换后端重试或无损迁移承诺。

## 不可变快照与 ABI v3

独立单程序`xdp_udp_rt`、section `xdp`；仅三个声明map，禁止额外/缺失/错形状map或程序。白名单不是恶意BPF的语义审计，只加载可信项目对象。

| map | 类型 | key/value字节 | 容量 | flags |
| --- | --- | --- | --- | --- |
| l4lb_active_v3 | ARRAY_OF_MAPS | 4/4 | 1 | 0 |
| l4lb_tpl_v3 | ARRAY | 4/1048 | 1 | BPF_F_RDONLY_PROG |
| l4lb_stats_v3 | PERCPU_ARRAY | 4/64 | 1 | 0 |

所有key为u32的0。template为内层形状模板，清零freeze；实际快照每次新建独立inner，metadata与template相同。Clang18 BTF用单元素Snapshot数组保留完整嵌套值类型，实际大小仍1048。

Snapshot布局：schemaVersion u32@0=3、backendCount u32@4、generation u64@8、vipAddress u32@16、vipPort u16@20、reserved u16@22=0，64个v2格式目标@24，总1048字节、对齐8。目标每项16字节，未用槽清零；地址/端口网络序，其余整数本机序。C/BPF与C++共同断言布局。

候选配置/探测资源准备→新建inner→完整写/读回→freeze及只读验证→单次outer[0]更新。**outer更新成功是提交点**；此前失败不动活动入口，之后不得假称回滚。提交成功后观测/日志等致命错误应条件卸载并退出1。

BPF每包仅lookup一次outer，全部VIP/count/backend读取来自同一inner。旧inner不原地复用；用户态关闭自持fd后由内核引用和RCU退休，不用猜测sleep宽限期。可查询map ID消失不等于物理内存已经立即释放。机制依据见[Linux map-in-map文档](https://docs.kernel.org/bpf/map_of_maps.html)，实际环境仍需真实内核并发验证。

每秒至多一次发布尝试，初始generation=1除外；单控制线程合并变化，持有活动与一个候选fd，内核退休可能延迟。generation不回绕，统计独立于快照且不因发布归零。生产无Python、线程或持久pin。

## 输出、停止与故障

READY包含`schema=3 profile=udp-dsr-runtime generation=1 configured_backends=N active_backends=0`及dev/mode/prog_id；它表示初始化和挂载完成，不表示已有健康后端。

- `XDP_RELOAD status=applied|unchanged|rejected`表示文件事务结果。
- `XDP_HEALTH target=ID from=Unknown|Healthy|Unhealthy to=... reason=...`表示健康状态变化。
- `XDP_PUBLISH status=applied|failed reason=reload|health generation=G active_backends=M`中G/M始终描述已应用快照，不能把desired集合冒充已生效。
- `XDP_STATS schema=3`保留v2八计数：total_packets、pass_packets、redirect_requests、drop_packets、unsupported_packets、no_backend_packets、invalid_config_packets、helper_error_packets。

INT/TERM/SIGPIPE停止优先于HUP，停止后不再发布；先条件卸载，再取消探测、读统计及输出DETACHED。新profile输出使用非阻塞64KiB上限队列和单轮写预算；满队列、broken pipe或输出描述符错误清理退出1，不能无限卡住健康循环。停止后输出最多等待1秒。

参数错误退出2、运行错误退出1、正常停止退出0。外部替换不能被误卸载；SIGKILL可能残留程序及活动maps，须确认当前program ID后使用原detach恢复。不会自动修复出口、邻居、路由或后端服务。

本阶段的源文件、产物指纹及双方实测结果见[S3验证摘要](../runbooks/xdp-runtime-validation-result.json)；原始日志位于摘要列出的本地临时目录，普通clone可按本手册复现。
