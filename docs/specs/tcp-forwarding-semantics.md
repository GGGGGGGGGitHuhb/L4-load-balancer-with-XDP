# TCP 转发语义（V0.1/S2）

本规格描述 S2-D1 转发与 V0.3-S1-D1 健康资格。仅支持数字 IPv4、TCP 用户态单线程代理，不解析应用协议，不保留客户端源地址，不提供 TLS 或失败重试；UDP 有独立规格。

## 运行与调度

- `l4lb --run <path>` 完整校验现有 listen/backend 配置后启动前台服务；监听器及 epoll/signalfd 就绪后 stdout 输出 `TCP 服务已启动：<IPv4>:<port>` 并立即刷新。
- `--help`、`--check-config`、`--run` 互斥；无参数、缺路径、多余或重复选项退出 2；配置和启动系统错误退出 1。配置检查行为保持 S1 兼容。
- 第一个接纳会话选择第一个 backend，之后按配置顺序循环。会话一生绑定一个后端；连接失败也消耗一次轮询位置，不重试。达到容量拒绝的新连接不消耗位置。
- 后端 socket 非阻塞 connect；EINPROGRESS 后通过事件查询 SO_ERROR 确认结果。Connecting 不读取客户端应用字节。真实连接错误记录操作及 errno，只关闭该会话。

## 字节、缓冲与事件

- 单线程 LT epoll；accept 每批至多 64 个，单方向 recv/send 每次处理各至多 64 KiB，截止和信号处理位于事件前后。
- 两个方向各自保存至多 64 KiB 未发送字节；send 成功多少就消费多少，短写尾部保留。消费空间在后续 recv 前回收。
- 达到 64 KiB 暂停源 IN/RDHUP；降至 32 KiB 或以下恢复并主动尝试读取。反向队列及读写独立，源暂停不禁止向该端写入响应。
- 只有队列非空才订阅目标 OUT，Connecting 订阅 OUT 为独立需求。空且无需读的 endpoint 暂不注册，避免 EOF/HUP 的持续就绪。
- HUP/RDHUP 不直接代表可丢弃队列；recv=0 才确认正常 EOF。无进展 HUP 暂挂 endpoint，100ms 后主动尝试两个方向并重算订阅；不会永久丢弃反向写入机会。
- EINTR 重试并检查停止，EAGAIN/EWOULDBLOCK 不视为断开。send 使用 MSG_NOSIGNAL；ERR 查询 SO_ERROR；致命 I/O 错误关闭一整对连接。
- endpoint 以单调 token 标识；关闭先使 token 失效，再 DEL 和释放 fd。同一批中过期事件不能命中新复用的 fd。

## 关闭和超时

- 单向 recv=0 后停止该向读，排空对应队列再向目标 SHUT_WR。目标收到 EOF 后仍能返回完整响应，反向先 EOF 也适用。
- 两端均 EOF、队列已空且必要 SHUT_WR 完成才以 `drained` 正常结束。RST、SO_ERROR、致命 recv/send/shutdown 记异常原因，可丢弃待发送队列，绝不记正常排空。
- Connecting 截止 5 秒；Established 连续 60 秒没有正字节 recv/send 则关闭。使用 steady_clock，EAGAIN 或纯就绪不能续命；epoll 等待上限 100ms，容许调度误差。
- SIGINT/SIGTERM 停止接入并立即清理会话，退出 0，stderr 输出停止摘要；不保证在途数据排空。不恢复已经失败的会话或等待后端完成业务。

## 资源与日志

- 最大 1024 个存活会话（含 Connecting），每个两个 fd 和两条有界队列。应用待发数据上限为 128 MiB；内核 socket 缓冲及对象/容器开销另外计算，不代表进程内存上限。
- 监听 backlog 128、SO_REUSEADDR；accept4 使用 NONBLOCK/CLOEXEC。容量满时 accept 后立即关闭。accept 的 fd/内存资源错误暂挂监听 100ms，连续错误只给一次诊断；其他不可恢复监听/事件循环错误清理后退出 1。
- fd 使用不可复制、可移动 owner；服务退出恢复调用线程原始信号 mask。
- stderr 每会话最多一次 `accepted` 和一次终止摘要：session ID、backend、reason、双向成功 send 字节，错误附 errno。没有载荷或逐 recv/send 日志，不是指标服务。
- 观测回调与故障注入只在内部 C++ 测试驱动使用，产品无测试 CLI、环境开关或额外日志。

## 验证入口与证据解释

`ctest --test-dir build-s2 -L s2 --output-on-failure` 包含状态单元、真实 TCP 进程集成和定向 reactor 状态测试。

真实集成记录 EINPROGRESS、事件、SO_ERROR=0/ECONNREFUSED；两向 8 MiB+333 字节慢读使用真实回环及较小内核发送缓冲，验证逐字节一致、EAGAIN/短写、64KiB 高水位及暂停恢复。220次连接观察 `/proc/<pid>/fd` 返回基线和 session/token 归零。

确定性补充分开记录：7字节 send 包装仍调用真实 send；内部零连接截止触发真实 Connecting 超时路径，250ms 空闲截止触发 idle 路径，单测验证生产5秒/60秒默认边界；EMFILE 注入验证监听退避。满队列 HUP 定向注入使用 socketpair 和同一 reactor 实现，验证100ms前不重试、反向进展与恢复；不声称该 HUP 来自自然回环 TCP。强制 fd 复用后投递旧 token 验证同批次隔离。

不作吞吐性能承诺；超时/资源注入不是公网故障或 root 防火墙实验。演示 fixture、全部配置及测试记录均为本机用途。

## V0.3/S1 可选健康资格

`health_check` 默认 off，原行为保持。显式 tcp_connect 只检查相同后端端口 TCP 握手，初始 Unknown 不可选，连续2成功开放、3失败摘除，固定1s间隔/1s超时。ready 不等待 Healthy。新 client 无可选后端立即关闭（EOF/reset），不建 backend socket/会话，不 fallback；全不可选限频输出 no_healthy_backend，服务保留等待恢复。既有 TCP 会话不受健康状态主动中断，仍按原网络错误/空闲时限退出。探测本机资源不足可能保守摘除，详见 [健康规格](health-check.md)。

## V0.3/S2 可选指标

metrics默认off；stderr模式将真实创建/关闭、成功send提交量、拒绝/drop/error/timeout累计为固定schema快照。TCP计入connecting会话，send每次正返回即时计字节，不等关闭；UDP完整成功一包计一次，零长包计包不计字节，recv失败无虚构drop。旧健康不eligible的flow仍可贡献提交量。错误在日志限频前计，关闭不重复计原错误；成功提交不保证对端收到。同步stderr风险、尾快照和全部字段见 [metrics规格](metrics.md)。
