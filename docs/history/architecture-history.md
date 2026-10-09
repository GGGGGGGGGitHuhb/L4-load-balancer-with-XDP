# 历史架构记录

本页归档自 `refactor-r4` 的 ARCHITECTURE.md 历史章节。旧内部路径、测试数量以及“当前”“未发布”等表述，只描述对应版本的原时点，不表示现在的实现或发布状态。

现行模块、数据流和所有权见[系统架构](../../ARCHITECTURE.md#current-source-index)。迁移保留原记录内容，仅调整因文档位置变化而需要更新的相对链接。

## V0.1/S2 历史落地边界

- `core/round_robin.h` 是固定顺序纯索引逻辑；`control/tcp_service.*` 组装选择回调与结构化会话日志；`net/reactor.*` 独占 LT epoll、会话与信号/截止，产品保持单线程。
- `net/fd.h`、`net/state.h` 承担 fd owner、有界缓冲、截止与 endpoint token。网络层复用已校验 Endpoint 类型，不读取配置或自行选后端。
- 本阶段只实现 TCP 静态轮询；上文健康检查、指标、UDP/XDP 为长期架构，不表示当前已实现。用户语义以 `docs/specs/tcp-forwarding-semantics.md` 为准。

## V0.1/S3 历史验证边界

- `tests/tcp_product_smoke.cpp` 通过独立进程启动真实产品与 echo fixture，不链接生产 reactor/control 内部；CMake 仅在 BUILD_TESTING 下生成该入口和 fixture，生产依赖不变。
- fixture 动态端口、原子证据目录、有界 ready/I/O、明确进程回收和端口重绑用于可重复验收；它们不改变正式产品配置或 TCP 参数。
- V0.1 开发范围已独立验收完成；复现入口见 `docs/runbooks/local-tcp-validation.md`，七条标准见 `docs/specs/v0.1-acceptance.md`。本地 main 已包含 S3 合并提交 544c8d8；远端发布状态本轮未核验，后续架构方向不代表当前已实现。

## V0.2/S1 历史落地边界

状态：Completed（2026-09-08）；Reviewer002 PASS、Leader003 收尾。以下为 S1 验收时的实现事实，后续变化见 V0.2/S2。

- `config` 新增强类型 Protocol/SchedulerKind 和兼容默认值；`control/service.*` 在网络资源创建前分派协议，当前 UDP 明确拒绝运行。
- `core/scheduler.*` 提供纯索引策略接口与工厂，RoundRobin 实现它；`control/tcp_service.*` 独占 scheduler 并将选择回调传给 reactor，生命周期覆盖整个 run。reactor 不解析协议或策略。
- 单 listener、静态有序池、独立 cursor 不变；失败消耗选择、不重试。无 UDP socket/flow table、健康检查或第二策略；规格见 `docs/specs/scheduler.md` 和 `docs/specs/config-schema.md`。

## V0.2/S2 历史落地边界

- `core/udp_flow.h` 定义纯 FlowKey/空闲时限；`net/udp_reactor.*` 持有每 flow 后端 socket、双索引、单调 token 和共享接收 scratch，独立于 TCP reactor。
- `control/udp_service.*` 独占 Scheduler、记录生命周期并接入统一协议分派；UDP 在完整数据报/元数据通过且容量允许时为新 flow 选择一次，回复使用 listener IP_PKTINFO 固定实际目的 IP 为源。
- 1024 flow、60 秒空闲、64 次每 fd 接收尝试、100ms 清扫，立即发送/整包丢弃，无用户态队列；真实数据包和局部注入证据分别由 UDP 状态测试、独立产品测试承载。具体错误、wildcard、零长/截断、迟到包和安全限制见 `docs/specs/udp-flow-table.md`。
- S2验收时为 Completed（2026-09-08），Reviewer001 独立 PASS、Leader003 收尾；当时S3尚未开始，当前S3测试边界见下节，无 UDP 可靠性、性能或公网防护承诺。

## 变更记录

- `2026-09-11`：根据用户授权与环境预检改用本地 XDP 验证路线，取消云服务器前置要求；模块边界保持。

- `2026-05-21`：补充项目环境路线，明确 WSL2 用于用户态开发，云服务器用于 XDP/eBPF 功能验证与阶段收尾。
- `2026-05-19`：创建初版架构文档，明确 C++ 用户态 L4 负载均衡器、控制面、用户态数据面和 XDP/eBPF 数据面的长期职责边界。


## V0.2/S3 历史测试边界

历史收尾时状态：Completed（2026-09-09），Reviewer001独立PASS、Leader003收尾；V0.2开发范围完成，生产数据面未改，当时未发布S3。

- 生产src/与原TCP示例保持S2合并版本。扩展 `tests/udp_product_test.cpp` 的system/expiry模式，复用独立argv、原子证据目录与有界进程管理，新增CTest两项，不链接生产control/net或注入Options。
- P1/P2验证原产品wildcard/实际源地址/流隔离/伪造过滤和后端停机后显式新flow恢复；P3用同一socket真实静默≥60.5秒验证原60秒默认值，不调整产品常量或时钟。
- 手动工具 `tests/udp_manual.py` 仅用Python3标准库，客户端持续持有同一socket；`tests/udp_manual_demo.sh`提供可复制完整流程和自有子进程/端口清理。当时Python不成为生产运行或CTest依赖；V0.3起测试构建使用Python，生产仍不依赖。
- 容量1024静态确认，内部capacity2动态继承；无1024满载/性能结论。当时11项注册、Debug快速10项、Release11项含长项一次；完整边界见UDP运行手册和V0.2完成矩阵。

## V0.3/S1 历史落地边界

- `health/state.h` 纯状态机，`health/checker.*` 单线程非阻塞 probe，独占 epoll/fd/token/steady deadlines，最多256后端。每次完成后1s、超时1s，旧token先撤销再close，截止优先；checker故障清理后传播为服务错误。无线程、sleep 或应用payload。
- `control/health_selection.*` 持有原 Scheduler 和可选 checker、汇总转换日志，先检查集合再有界跳过，不让 health 依赖 control 或直接断业务。off 无 checker/fd/maintenance。
- TCP/UDP `select_backend` 返回 optional Endpoint，maintenance 每轮唤醒后、停止判断后、分派前调用；长批次新选择前再次tick。net 不读取 health_check，不解释健康状态。无可选时分别关闭新client/丢弃新key，旧会话/flow 原绑定保持。
- UDP 的 TCP 探活是操作员提供的代理信号，不是 UDP 协议健康；Unknown 预热拒绝新业务，ready 不代表 Healthy，资源失败标明 local_error。完整语义见 `docs/specs/health-check.md`。
- CTest 新增4项至15项，Python3标准库仅用于真实产品 fixture（测试构建需 Python3，Production不需）。V0.2注册保留，Debug14快速、Release15含60s expiry；独立验收状态由角色报告记录。无 S2 指标快照/S3完整故障矩阵。

## V0.3/S2 当前指标边界

- `net/statistics.h` 为可选强类型事实回调，不接入测试Observation或限频日志。两reactor在真实事务提交/关闭/send/错误分支发事件，net不理解metrics开关或JSON。
- `metrics/metrics.h/.cpp` 固定Collector/Snapshot、纯schema=1格式化与Output；control的MetricsService独占它们，健康采集为HealthSelection只读复制，不tick或移动cursor。各模块继续.h/.cpp同目录。
- off无collector/output/回调/周期；stderr模式ready后1s维护，健康先行，reactor释放后单final/error。最多256backend与32768字节完整行，单线程同步stderr、不建异步队列。慢sink、SIGPIPE、半行与失败禁用边界见metrics规格，不承诺非阻塞日志。
- 新增5项正式模型/两reactor落点/两协议产品指标测试，保留原15项；当前20注册，Debug快速19、Release完整20。未提前实现S3完整故障矩阵/Prometheus/XDP。

## V0.3/S3 测试边界

新增Python标准库双backend fixture、三项真实产品CTest及独立工具负向。fixture区分健康空连接与业务nonce，以backend观测建立期望账本，再比较产品指标；测试线程/PID/socket由单run拥有。原metrics产品模块仅加main入口保护供schema解析复用，默认执行和既有断言保持。当时生产src、配置和模块边界不变，共23项注册；执行见 `docs/runbooks/local-v0.3-validation.md`。

## V0.4/S1 Benchmark 工具边界

`tests/benchmark_runner.py`负责有界生命周期、ready/配置和独立run证据；`benchmark_fixture.py`为独立echo进程；`benchmark_traffic.py`负责TCP闭环partial I/O与UDP总pps节拍/有界pending；`benchmark_stats.py`提供纯统计；`benchmark_environment.py`负责环境白名单和每PID测量窗口资源采样。工具只使用Python3标准库，不进入生产依赖、不改变src/配置/调度/健康/metrics。`benchmark_tool_test.py`注册3项短CTest（原23项保留），`benchmark_acceptance.py`提供显式长矩阵、失败和隔离验证。公开口径与schema见 `docs/benchmarks/methodology.md`；不提前实现生产优化或XDP。

## V0.4/S2资源与停止边界

Buffer保留初始化的固定64KiB数组，用head/size和连续读写span代替memmove压缩；net I/O同时受span/容量/budget限制。TCP用Running/Draining/Stopped逻辑状态，首次消费信号固定1s deadline并使listener失效，内层pump只标记停止、在安全调度边界移除session，避免悬空引用。Draining冻结新recv/连接和maintenance，仅发送已有pending；control通过StopEvent输出一次生命周期队列/截止摘要，不新增metrics schema或配置。TCP/UDP从索引中移出owner、DEL/close后才独立通知，显式清理传播首异常、noexcept析构清理所有owner。新增2项有限CTest及公开runbook，原26项保持；S1方法和样本身份不重写，兼容验证不作S3性能结论。

## V0.4/S3 对照工具边界

`v04_benchmark_identity.py` 从固定Git对象导出与构建产品并核验manifest，`v04_benchmark_compare.py` 串行编排同一最终runner，`v04_benchmark_data.py` 独立重算原始统计/完整格点并生成可移植数据包。product_identity与工具工作树environment分开；产品src/configs保持S2，正式矩阵不进入CTest。公开报告与版本标准入口分别在 `docs/benchmarks/reports/v0.4-user-space.md`、`docs/specs/v0.4-acceptance.md`，阶段完成仍需独立Reviewer和Leader收尾。

## V1.1/S1 BPF 构建边界

`src/xdp/xdp_pass.bpf.c`仅包含Linux UAPI与最小XDP_PASS入口；`cmake/Xdp.cmake`在L4LB_BUILD_XDP开启时探测BPF工具链并生成独立object。默认OFF，不向用户态目标传播BPF依赖、编译选项或链接对象。BUILD_TESTING开启时新增无特权xdp_build对象检查。S1交付时无项目loader、maps或attach调用；S2新增loader如下，maps仍未实现。具体构建入口见 `docs/runbooks/xdp-build.md`。

## V1.1/S2 加载器边界

`l4lb-xdp` 独立于既有l4lb：`main.cpp`严格解析attach/detach设备/模式/对象或ID，先阻塞退出信号；`loader.cpp`使用官方libbpf加载并挂载，前台同步sigwait后条件卸载。使用RAII管理object/FD，不引入回调注册或业务装配层。程序通过内核old_prog_fd原子比较保护其他挂载；比较FD复制到>=3，防止FD0被libbpf视为未指定。READY输出失败亦清理；SIGKILL不能自动清理，需要用户提供ID显式卸载。

`L4LB_BUILD_XDP_LOADER`默认OFF，仅开启时检测libbpf>=1.0，且要求S1 BPF构建开启；不传播到原用户态目标。默认CTest只增加普通UID的CLI负向检查，真实BPF验收是显式root自建net namespace/veth脚本。无maps、持久pin、TCP代理或性能承诺。运行契约见[加载手册](../runbooks/xdp-loader.md)。

## V1.2/S1 map 与启动同步边界

独立 `xdp_maps.bpf.o` 与旧 `xdp_pass.bpf.o` 并存，二者始终 XDP_PASS。`MapSchema.h` 定义 cfg ARRAY、64项backend ARRAY和单项PERCPU_ARRAY统计；固定宽度布局由C/BPF与C++共同断言，不承载会话或用户态对象。

`control/XdpConfigSync`负责参数转换及完整写入/回读/冻结次序；`xdp/MapStore`负责ABI白名单、内核metadata、读写和统计汇总；`Attachment`持有object/fd并在同步完成后才挂载。配置发布前无包路径读者，发布后冻结，不引入并发热更新或健康状态联动。CLI呈现READY、错误及独立XDP_STATS，原用户态metrics不变。

新增代码仅链接到可选 `l4lb-xdp`。BPF-only不需要libbpf开发包，默认OFF不探测XDP依赖。正常停止先条件卸载再读统计；SIGKILL残留按实际program ID显式恢复，没有pin或他人map写入。正式布局、非原子统计采样和兼容界限见[map schema](../specs/xdp-map-schema.md)，验证入口见[XDP流程](../runbooks/xdp-validation.md)。

## V1.1/S3 验证与文档边界

S3复用S1/S2构建和测试，不新增运行时层。默认OFF、BPF-only、独立loader三种组合及普通UID/特权分层见[V1.1最小验证](../runbooks/xdp-validation.md)。特权脚本直接在临时net namespace中注入Ethernet/IPv4/UDP帧，验证真实挂载和条件卸载，普通用户态测试在隔离网络降权运行。

[map schema初稿](../specs/xdp-map-schema.md)明确V1.1实际map集合为空；V1.2候选后端与统计字段只是后续设计输入，不是共享ABI或已实现控制面同步。版本六标准与记录环境见[验收索引](../specs/v1.1-acceptance.md)，公开JSON用于审计本轮结果，不作为性能或跨平台保证。

## V1.2/S2 UDP DSR 边界

`xdp_udp_dsr.bpf.o`使用独立schema v2，与旧PASS和schema v1对象并存。包路径有界解析Ethernet/IPv4/UDP，校验IPv4头及长度，按五元组FNV-1a选择静态目标，通过`bpf_redirect`送往出口；仅改二层MAC，后端以VIP直接回包。未支持、非目标、无后端或坏配置PASS，helper即时失败DROP；重定向请求不代表实际送达。

`control/DsrConfigSync`负责VIP/目标解析与写入、完整回读、冻结；`xdp/DsrMapStore`负责v2白名单、内核map访问及八项per-CPU计数汇总。加载器检查入口/出口Ethernet接口、状态、MAC和MTU，先发布完整只读配置再挂载；停止先条件卸载再读统计。原用户态代理、metrics和S1同步路径保持。

后端VIP、回程、邻居由部署方配置，产品不改宿主网络。无会话、NAT、热更新或健康联动；运行期联动留S3，性能留S4。正式ABI、解析边界和统计语义见[DSR规格](../specs/xdp-udp-dsr.md)，独立namespace/veth复现见[XDP验证](../runbooks/xdp-validation.md)。

## V1.2/S3 运行期 DSR 控制面

新增第四种独立profile：`xdp_udp_runtime.bpf.o`读取schema v3的单个ARRAY_OF_MAPS活动入口，每包只取得一次不可变inner快照；完整VIP、代次、计数和64槽目标处于同一1048字节值中。`RuntimeMapStore`创建、写入、回读并冻结新inner，再用一次outer更新提交；旧快照不原地复用，由内核引用/RCU退休。统计map独立，不随配置发布清零。旧v1/v2契约不变。

`RuntimeDsrConfig`负责严格有界文件读取、纯文本解析及接口/目标身份解析；`UdpProbeChecker`负责单线程非阻塞UDP echo、deadline/nonce和2success/3failure状态，不读取BPF或输出日志；`RuntimeDsrService`串行编排HUP事务、desired健康集合和已应用generation，至多每秒尝试一次发布。候选探测资源和状态在提交前准备，提交后只做无抛出所有权替换；后续致命观测/日志失败终止卸载，不假称回滚。

runtime用signalfd和20ms有界轮询收割响应，优先消费停止信号；输出以64KiB非阻塞队列限制背压。全部不可用时仅新profile对有效匹配报文DROP；配置/健康改变可能重映射既有UDP流，不引入会话或draining。部署方提供UDP echo端点、VIP和回程，产品不配置网络。ABI、探测与提交点语义见[运行期控制规格](../specs/xdp-runtime-control.md)。S4负责性能，本阶段不作吞吐或物理网卡结论。

## V1.2/S4 性能验证工具边界

新增 `tests/v12_benchmark*.py` 仅在测试侧构建固定S3 Release产品、建立自有隔离网络、按开放节拍生成UDP流量、采集原始计数/RTT/PID与全机资源并离线重算；CMake只在BUILD_TESTING内增加短schema测试。生产模块、ABI、历史测试及配置保持冻结，无新生产Python依赖。工具记录漏槽和超时，不以目标pps代替实际goodput；全机CPU含VM背景任务，loader CPU不是BPF数据面CPU。六路径与控制面测量的拓扑/语义和口径见[性能方法](../benchmarks/v1.2-methodology.md)，独立验收与九标准见[版本验收](../specs/v1.2-acceptance.md)。此段登记已批准S4落地，不变更产品架构或将后续平台纳入验收。
