# UDP flow table（V0.2/S2–S3）

`protocol=udp` 使用单线程 LT epoll 提供尽力数据报转发。无新配置键，`round_robin` 仍为唯一策略。本规格描述当前实现；S3 补充独立产品场景和完整公共手册，未改变生产语义。

## 关联、地址与资源

- `core/udp_flow.h` 的 FlowKey 是客户端 IPv4/UDP 端口加实际本地目的 IPv4；listener 端口及 UDP 协议由单实例隐含。相同源端点向 wildcard 的不同目标地址发送会建立不同 flow。
- listener 为非阻塞/CLOEXEC IPv4 UDP socket，一次 bind，无 listen/accept、SO_REUSEADDR/SO_REUSEPORT/SO_BROADCAST。无论具体地址或 `0.0.0.0` 都必须启用 IP_PKTINFO，失败则启动失败、无 ready。
- recvmsg 验证完整 AF_INET 客户端、非零源端口、非零单播源/目的地址、无 MSG_CTRUNC，恰有一个完整 IP_PKTINFO。拒绝 224.0.0.0/4、255.255.255.255、`ipi_addr != ipi_spec_dst`；具体绑定还要求目的地址匹配。缺失、截断或不支持元数据在建 flow/调度前整包丢弃。
- 保存 `ipi_addr` 为 flow 的目的 IP。回复从原 listener 以 sendmsg 发回客户端，IP_PKTINFO 设置 `ipi_spec_dst=保存地址`、`ipi_ifindex=0`、`ipi_addr=0`，保持源 IP/源端口与请求目标一致；不复制接收 ifindex 干扰路由。特殊策略路由/地址重配不在保证范围。
- 每个 flow 独占一个非阻塞/CLOEXEC UDP backend socket，先 bind `INADDR_ANY:0` 再 connect 到选定静态后端；无握手/探活，connect 成功不表示可达。后端看到代理临时端口，不是原客户端端点。
- 后端请求使用 send，回包使用 recvmsg，仅接收已连接对端的源 IP/port；外来 socket 不能通过回包覆写关联。一个请求允许多个回包，不解析 request ID，不在用户态提供发送队列。
- `net/udp_reactor.*` 持有 key→token 与 token→flow 索引；flow 内持有 fd RAII，core 只保存纯值类型。控制层持有 Scheduler，生命周期覆盖同步 reactor，不将配置策略解析或日志放入 net。

## 建立、调度与容量

- 生产固定最多 1024 个 flow。每轮先处理停止和到期，访问同键时再次检查过期；未过期命中直接复用后端，不推进调度，即使容量已满也可继续。
- 未命中先清理到期项再检查容量。满表整包丢弃，不驱逐活跃 flow，不分配 socket，不推进调度。
- 容量允许后选择恰好一次，按配置次序 A/B/A。创建、bind、connect、token/epoll 注册及索引插入构成事务；资源失败清理部分 owner/索引，已消耗选择不回滚，不对原包重试或改选。选择回调异常是控制契约失败，清理服务并向上报告。
- UDP connect 任何非零结果（包括 EINTR/EINPROGRESS）都属于建立失败，不进入 TCP 式 Connecting 状态。下一新数据报可以重新建立，不等于重试原包。
- 新建时初始化 last_activity，即使首包 EAGAIN 也仍受时限约束；成功建立后立即尝试发送首包。
- 用户态一个共享 65507 字节 scratch buffer，无每 flow 数据队列；表/token/fd 随 flow 数有界，另有 listener/epoll/signalfd 固定开销。元数据 O(max_flows)，过期扫描 O(max_flows)。内核 socket buffer 由 OS 管理，不承诺固定 RSS；fd/临时端口配额可能提前限制容量，不修改系统限额。

## 数据报、丢弃与时间

- 双向 payload 最大 65507。一次 recvmsg 对应一个数据报；0 是合法零长包，不是 EOF。完整成功发送零长也刷新活动。不会粘包、拆包或分次补发短写尾部。
- MSG_TRUNC 或长度超过上限整包丢弃，不转发前缀、不刷新时限；listener 上也不建立 flow/推进调度。backend 截断只丢当前包，flow 保留。
- 每包立即尝试发送，返回值必须等于原包长（含 0）。短写整包丢弃且不补发。EAGAIN/EWOULDBLOCK/ENOBUFS/ENOMEM/EMSGSIZE 保留 flow、整包丢弃、不排队/不重试。EINTR 最多额外尝试三次，总计四次，然后丢弃。
- 无 EPOLLOUT 兴趣，不调整 PMTU discovery，不自行分片；发送成功只表示内核接收，不代表最终交付、可靠性或顺序。
- 每 fd 每轮最多 64 次接收尝试，EINTR 也计数；epoll 批次最多 128。LT 下剩余数据下轮继续，信号优先、批次间过期扫描，防止热 fd 无限占用循环。
- steady_clock 空闲 60000ms，`now-last >= timeout` 即到期。两方向完整包成功提交发送（含 0）才更新 last_activity；收到、丢弃、短写、失败发送均不更新。默认清扫间隔 100ms 加调度延迟，访问时额外检查过期，旧事件不能复活到期 flow。
- 后端有效回复也可保活；这不是抗恶意保活机制。超时后同键的新请求重新消耗选择；无半关闭、排空或合成协议错误响应。

## 错误、身份和停止

- backend EPOLLERR 先读 SO_ERROR。压力/EMSGSIZE 保留 flow，其余非零网络错误如 ECONNREFUSED/ENETUNREACH 关闭当前 flow；SO_ERROR=0 继续读取。ERR|IN 一旦关闭立即返回，不继续通过旧引用接收。异常 HUP 或读取 SO_ERROR 失败也关闭当前 flow；全局坏 fd/内部不变量错误向上传播。
- backend recv/send 非临时网络错误同样仅关闭该 flow。listener 是共享 socket，其异步错误无法精确归属当前客户端；ECONNREFUSED/ECONNRESET/ENETUNREACH/EHOSTUNREACH/EMSGSIZE/EACCES/ENOBUFS/ENOMEM 等可恢复错误只限频诊断并继续，不随意删除 flow；其余不可恢复错误清理服务。共享收发错误同样分类，已消费数据报不重发。
- 不启用 IP_RECVERR，不将 ICMP 翻译给客户端。ICMP 可能延迟或缺失；无探测、惩罚、重传、失败切换或自动跳过后端。
- flow token/ID 单调不重用，耗尽显式失败。删除索引先于 epoll DEL/fd close，旧批次只查 token，实际 fd 复用也不能使旧事件命中新 owner。
- token 只防用户态旧事件。OS 未来复用 UDP 端口/对端元组时，网络迟到包可能属于新映射；不承诺跨超时应用事务隔离、抗伪造或冷却池，业务应自行携带应用 ID。
- signalfd 消费 SIGINT/SIGTERM 后退出 0；全部 flow/listener/epoll/signalfd 由 RAII 关闭并恢复原信号掩码。启动资源失败无 ready、退出 1；不可恢复运行错误同样退出 1、释放全部资源。没有后台线程。
- 控制层记录启动/停止和 flow 创建/结束（id/backend/reason/errno），不记录载荷。每包丢弃、满表、压力类诊断每类别每秒最多一次；只读测试观察不等同公开指标。

## CLI 与测试边界

- `--check-config` 保持纯静态、无 socket 和原成功文本。
- `--run` UDP ready 为 `UDP 服务已启动：<配置地址>:<端口>` 加换行并立即刷新，全部初始资源就绪后才输出。停止 stderr 为 `UDP 服务已停止：全部 flow 已关闭（不保证在途数据排空）` 加换行。
- `v02_udp_state` 包含 key/受控时钟单元、真实 socket 的关联/双向 0..65507/截断/wildcard/ICMP/fd 复用/热流量测试，和明确标注的元数据、收发错误、建立事务及旧 token 批次注入。
- `v02_udp_product` 独立进程执行实际产品，验证完整数据报、按 flow A/B/A、ready、占用端口和 SIGINT/SIGTERM；Release 三种负向测试模型为错误 payload、fixture 将零长当 EOF、错误后端选择期望，目标断言必须失败且回收子进程。
- UdpOptions 仅内部测试使用，可缩小容量/超时/接收缓冲、注入局部 syscall/只读观察/单调时钟；默认是真实 syscall。容量 0、非正时间、超过 int 的 poll 时间、接收缓冲 0 或超过 65507、缺失选择回调均拒绝；不暴露新配置或 CLI。
- 若执行环境将 127/8 路由经代理接口导致大包丢弃，可在临时 user/net namespace 的独立 lo 验证；不能把环境大包丢弃当成产品 PASS，不能为了测试改变宿主路由。

## 适用限制

受控 IPv4 用户态实验代理，无认证、源地址反欺骗、限速、DDoS 防护或公网 relay 承诺。静态后端限制目的地，但 UDP 源伪造和后端放大风险仍可能存在。没有 IPv6、DNS、多 listener、多策略、持久化、热加载、性能结论或 XDP；S2 完成不代表 V0.2/S3 完成。


## S3 产品与版本验收分层

- 新增 `v02_udp_system`：原产品 wildcard 同源两目标、同后端不同客户端反序回复、第三socket伪造包及合法控制组；关闭后端A后，等待对应flow真实ECONNREFUSED关闭且旧包未到B，显式新请求到B，恢复A后新key到A且旧B保持。此路径没有探活或原包失败切换。
- 新增 `v02_udp_expiry`：原样Release产品，客户端完成A回复后静默至少60.5秒，再用同一个客户端socket/目标发新nonce到B；没有短时限/系统时钟注入。一次实测验证生产默认60秒被使用，不承诺精确实时清理；内部等于到期/零长刷新/丢包不刷新仍由S2状态测试覆盖。
- 生产容量默认1024为源码/规格静态确认；容量2的满表拒绝、不驱逐、不推进调度和到期准入由内部真实socket/受控时钟动态验证。没有1024满载产品测试，不将OS配额或该常量描述为吞吐/容量性能结果。
- `udp_fast`只含基础产品和system两项，可重复/并行；`udp_long`仅expiry长项，Debug快速不运行，Release最终一次。当前注册11项、Debug快速10项、Release全套11项。
- [完整UDP运行手册](../runbooks/local-udp-validation.md)提供普通clone可复现的固定客户端、nonce/零长/二进制、wildcard、停机/新flow恢复、错误端口和信号清理；[V0.2六条矩阵](v0.2-acceptance.md)区分继承、新增、静态默认值与未验证边界。
- UDP尽力丢弃、后端临时端口、ICMP可能延迟、OS元组复用后网络迟到包无法由用户态token辨认、无公网防护等限制保持。S3验证不增加可靠性、认证或发布承诺。

## V0.3/S1 TCP 代理健康信号

- 默认 off，UDP-only 服务完全兼容。显式 tcp_connect 需相同 IP/数字端口的 TCP 健康端点，且操作员保证其状态代表 UDP。UDP connect 不证明健康；无 TCP 端点会被摘除。启动 stderr 一次说明约定。
- Unknown 初始拒绝新 flow；2成功变Healthy，3失败变Unhealthy，完成后1s/超时1s。本机资源失败按 local_error 分类，可能保守摘除。
- 新 key 无 Healthy 则整包丢弃，不创建 flow/backend fd、不消耗轮询或 fallback；既有 flow 后续数据仍到原后端，状态改变不迁移/关闭。自然错误和60s到期继续沿用原规则，新 flow 重新过滤。
- 全不可选不退出，恢复可接新 flow；日志不代替 UDP nonce 数据证据。详见 [健康规格](health-check.md)。
