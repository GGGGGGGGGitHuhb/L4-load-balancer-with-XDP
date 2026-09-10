# TCP 转发语义（更新至V0.4/S2）

本规格描述 S2-D1 转发与 V0.3-S1-D1 健康资格。仅支持数字 IPv4、TCP 用户态单线程代理，不解析应用协议，不保留客户端源地址，不提供 TLS 或失败重试；UDP 有独立规格。

## 运行与调度

- `l4lb --run <path>` 完整校验现有 listen/backend 配置后启动前台服务；监听器及 epoll/signalfd 就绪后 stdout 输出 `TCP 服务已启动：<IPv4>:<port>` 并立即刷新。
- `--help`、`--check-config`、`--run` 互斥；无参数、缺路径、多余或重复选项退出 2；配置和启动系统错误退出 1。配置检查行为保持 S1 兼容。
- 第一个接纳会话选择第一个 backend，之后按配置顺序循环。会话一生绑定一个后端；连接失败也消耗一次轮询位置，不重试。达到容量拒绝的新连接不消耗位置。
- 后端 socket 非阻塞 connect；EINPROGRESS 后通过事件查询 SO_ERROR 确认结果。Connecting 不读取客户端应用字节。真实连接错误记录操作及 errno，只关闭该会话。

## 字节、缓冲与事件

- 单线程 LT epoll；accept 每批至多 64 个，单方向 recv/send 每次处理各至多 64 KiB，截止和信号处理位于事件前后。
- 两个方向各自保存至多 64 KiB 未发送字节；send 成功多少就消费多少，短写尾部保留。使用固定容量环形存储回收消费空间，不压缩搬移；size/room是总量，实际send/recv只使用readable_size/writable_size连续span并受本轮budget限制，append仅提交该写span内成功长度。
- 达到 64 KiB 暂停源 IN/RDHUP；降至 32 KiB 或以下恢复并主动尝试读取。反向队列及读写独立，源暂停不禁止向该端写入响应。
- 只有队列非空才订阅目标 OUT，Connecting 订阅 OUT 为独立需求。空且无需读的 endpoint 暂不注册，避免 EOF/HUP 的持续就绪。
- HUP/RDHUP 不直接代表可丢弃队列；recv=0 才确认正常 EOF。无进展 HUP 暂挂 endpoint，100ms 后主动尝试两个方向并重算订阅；不会永久丢弃反向写入机会。
- EINTR 重试并检查停止，EAGAIN/EWOULDBLOCK 不视为断开。send 使用 MSG_NOSIGNAL；ERR 查询 SO_ERROR；致命 I/O 错误关闭一整对连接。
- endpoint以单调token标识。关闭先从sessions移出owner、清除token，再完成两个端点DEL/fd释放，最后独立尝试diagnostic/session/Closed/observe通知并保留首异常。同批旧token不能命中新复用fd，重复close为no-op。

## 关闭和超时

- 单向 recv=0 后停止该向读，排空对应队列再向目标 SHUT_WR。目标收到 EOF 后仍能返回完整响应，反向先 EOF 也适用。
- 两端均 EOF、队列已空且必要 SHUT_WR 完成才以 `drained` 正常结束。RST、SO_ERROR、致命 recv/send/shutdown 记异常原因，可丢弃待发送队列，绝不记正常排空。
- Connecting 截止 5 秒；Established 连续 60 秒没有正字节 recv/send 则关闭。使用 steady_clock，EAGAIN 或纯就绪不能续命；epoll 等待上限 100ms，容许调度误差。
- 第一次消费SIGINT/SIGTERM进入Draining，steady_clock固定截止为当前时间+1000ms；关闭并移除listener，不再accept、选择/连接后端或读取任一方向新字节，取消Connecting会话（service-stop-connecting）。只尝试发送此时已在两条用户态pending队列中的数据。
- Draining只对有pending的方向订阅OUT，保留MSG_NOSIGNAL/短写/EAGAIN/64KiB公平预算和HUP有限重试；不恢复IN/RDHUP、不执行常规idle/connect截止、listener退避恢复或control maintenance。已有健康checker资源在control服务退出后RAII释放。
- 双向队列空即可service-stop-drained关闭，不等EOF、后端业务或内核在途输入；到固定截止仍有队列则service-stop-deadline释放，发送进展不延长期限。已进入Draining后消费第二次INT/TERM立即service-stop-forced，标准信号可能合并。
- 三种正常信号停止均退出0；真实不可恢复服务错误仍清理后退出1。1s仅限reactor逻辑，不保证同步stderr阻塞、SIGPIPE、回调不返回或OS调度下的硬实时退出。
- stderr先输出一次停止屏障`TCP draining pending_c2b=<总队列> pending_b2c=<总队列> deadline_ns=<steady_clock绝对纳秒>`，最终文案为`TCP 服务已停止：已尝试有界排空用户态待发队列，不保证在途数据送达`。client send成功不证明已进入用户态队列。

## 资源与日志

- 最大 1024 个存活会话（含 Connecting），每个两个 fd 和两条有界队列。应用待发数据上限为 128 MiB；内核 socket 缓冲及对象/容器开销另外计算，不代表进程内存上限。
- 监听 backlog 128、SO_REUSEADDR；accept4 使用 NONBLOCK/CLOEXEC。容量满时 accept 后立即关闭。accept 的 fd/内存资源错误暂挂监听 100ms，连续错误只给一次诊断；其他不可恢复监听/事件循环错误清理后退出 1。
- fd使用不可复制、可移动owner，close EINTR不重试；服务退出恢复原始信号mask。显式清理完成全部资源/必要通知后传播首异常，noexcept析构逐个清理所有owner并抑制通知异常，不覆盖原服务异常。回调不允许重入reactor，内部观察仅只读。
- stderr 每会话最多一次 `accepted` 和一次终止摘要：session ID、backend、reason、双向成功 send 字节，错误附 errno。没有载荷或逐 recv/send 日志，不是指标服务。
- 观测回调与故障注入只在内部 C++ 测试驱动使用，产品无测试CLI或环境开关；只有上述停止生命周期屏障，仍无逐I/O日志。

## 验证入口与证据解释

`ctest --test-dir build-s2 -L s2 --output-on-failure` 包含状态单元、真实 TCP 进程集成和定向 reactor 状态测试。

真实集成记录 EINPROGRESS、事件、SO_ERROR=0/ECONNREFUSED；两向 8 MiB+333 字节慢读使用真实回环及较小内核发送缓冲，验证逐字节一致、EAGAIN/短写、64KiB 高水位及暂停恢复。220次连接观察 `/proc/<pid>/fd` 返回基线和 session/token 归零。

确定性补充分开记录：7字节 send 包装仍调用真实 send；内部零连接截止触发真实 Connecting 超时路径，250ms 空闲截止触发 idle 路径，单测验证生产5秒/60秒默认边界；EMFILE 注入验证监听退避。满队列 HUP 定向注入使用 socketpair 和同一 reactor 实现，验证100ms前不重试、反向进展与恢复；不声称该 HUP 来自自然回环 TCP。强制 fd 复用后投递旧 token 验证同批次隔离。

不作吞吐性能承诺；超时/资源注入不是公网故障或 root 防火墙实验。演示 fixture、全部配置及测试记录均为本机用途。

## V0.3/S1 可选健康资格

`health_check` 默认 off，原行为保持。显式 tcp_connect 只检查相同后端端口 TCP 握手，初始 Unknown 不可选，连续2成功开放、3失败摘除，固定1s间隔/1s超时。ready 不等待 Healthy。新 client 无可选后端立即关闭（EOF/reset），不建 backend socket/会话，不 fallback；全不可选限频输出 no_healthy_backend，服务保留等待恢复。既有 TCP 会话不受健康状态主动中断，仍按原网络错误/空闲时限退出。探测本机资源不足可能保守摘除，详见 [健康规格](health-check.md)。

## V0.3/S2 可选指标

metrics默认off；stderr模式将真实创建/关闭、成功send提交量、拒绝/drop/error/timeout累计为固定schema快照。TCP计入connecting会话，send每次正返回即时计字节，不等关闭；UDP完整成功一包计一次，零长包计包不计字节，recv失败无虚构drop。旧健康不eligible的flow仍可贡献提交量。错误在日志限频前计，关闭不重复计原错误；成功提交不保证对端收到。同步stderr风险、尾快照和全部字段见 [metrics规格](metrics.md)。

## V0.4/S2验证

[生命周期运行手册](../runbooks/local-v0.4-lifecycle-validation.md)提供ring参考模型、异常通知矩阵、真实双向pending/1s截止/第二信号、健康指标组合、ASan/UBSan及目标负向。内部Options仅提供正数且不超过int毫秒表示范围的drain_timeout，生产固定1s且不新增配置。S3正式性能报告不在本阶段。
