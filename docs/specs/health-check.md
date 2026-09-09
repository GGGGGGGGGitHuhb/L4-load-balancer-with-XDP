# 健康检查规格（V0.3/S1）

实现依据 Approved V0.3-S1-D1。该能力用于新 TCP 会话和新 UDP flow 的资格过滤，不迁移或主动关闭已有绑定。S1 的实现证据不代替 Reviewer 验收。

## 配置和信号边界

- `health_check=off|tcp_connect`，默认 `off`；严格区分大小写、空值/未知值/重复均拒绝。off 不构造 checker、probe socket 或健康计时器，旧配置不变。
- `tcp_connect` 只向每个配置 backend 的同 IPv4/数字端口完成 TCP 握手，然后关闭；不发送应用 payload。可能产生后端 accept/关闭日志，不能证明应用业务可用。
- UDP 显式启用时，操作员必须提供同 IP/数字端口 TCP 健康端点并保证其状态代表 UDP 服务。只有 UDP 监听会因 TCP 失败而不可选；UDP connect 成功、无 ICMP 均不构成健康证据。启动 stderr 一次提示该约定。
- `--check-config` 纯静态，无 socket，不验证端点存在，成功输出/退出码不变。ready 表示基础资源和监听就绪，不等于已有 Healthy 后端。

## 状态和阈值

每 backend 独立 `Unknown/Healthy/Unhealthy`，初始 Unknown 不可选；重启重置。只有 Healthy 可选。

- 连续 2 次成功使 Unknown 或 Unhealthy 进入 Healthy。
- 连续 3 次失败使 Unknown 或 Healthy 进入 Unhealthy。
- 每次成功清 failures，successes 饱和于 2；每次失败清 successes，failures 饱和于 3。阈值前保持原状态。因此 Healthy 前两次失败仍可选，Unhealthy 第一次成功仍不可选。
- 业务 connect/send 失败不修改健康状态，也不回滚调度或重试原业务。
- 只在转换时输出 stderr：`health backend=<IPv4:port> from=<state> to=<state> reason=<reason> [errno=N]`；不逐 probe 打成功日志，不记录载荷。

## 时序、错误与资源

- 生产固定完成后间隔 1000ms、每次 timeout 1000ms，steady_clock；首次 tick 立即发 probe，每 backend 最多一个在途。单轮每 backend 最多一个完成，不追赶积压任务。
- 每次 tick 在收割事件后先检查当前 `now >= deadline`；晚成功不能覆盖超时。单调 token 在关闭前撤销，旧事件/fd 复用不记到新代次。
- socket 使用 NONBLOCK/CLOEXEC；connect=0 成功；EINPROGRESS 注册 EPOLLOUT/ERR/HUP，SO_ERROR=0 成功，其他失败；EINTR 保守计失败。每次完成 DEL/close，停止析构取消在途不计失败。
- 拒绝/不可达/timeout 为探测失败。EMFILE/ENFILE/ENOMEM/ENOBUFS、socket/SO_ERROR 系统调用或注册失败以 `local_error` 分类；本机压力可能导致保守摘除，不表示确定的后端自身故障。
- checker epoll 创建、非 EINTR 轮询失败或内部身份不变量错误向上传播，服务清理退出 1，不容许停探后永久 Healthy。
- 最多 256 个 probe socket 加一个 checker epoll；容器固定上界，单轮事件数组 256，扫描工作有界，无 sleep、阻塞 connect、线程或额外生产配置。

## 控制面和业务语义

control 持有 checker 和原 Scheduler，两个 reactor 在每轮唤醒、处理停止后且分派前执行可选 maintenance。长批次每次新选择再非阻塞 tick，资格不过期；单线程读写。off 不注册 maintenance。

- 空 Healthy 集合返回 nullopt，不调用 Scheduler::next，不推进 cursor；有 Healthy 时至多 next N 次，跳过不可选项，首次 Healthy 返回。恢复不重置 cursor。没有第二种策略或 fallback。
- TCP 接受 client 后无后端可选立即关闭，既不创建 backend socket，也不保留会话；EOF/reset 均可能。不健康转换不关闭既有连接。
- UDP 新 key 不可选整包丢弃、不建 flow/socket；既有未过期 flow 的后续包仍发到原后端。自然错误或原 60s 超时仍照旧删除，之后的新 flow 重新过滤。
- 容量、坏包早拒绝在选择之前；选定后资源/业务失败照旧消耗位置、不重试。
- 全部不可选时服务继续运行，每服务 `no_healthy_backend` 每秒最多一次；恢复后新业务可正常进入。“不接收新流量”只指新会话/flow，不指阻断旧 flow 的所有后续包。

## 验证层

- `v03_health_unit`：纯状态机 4096 个 12步序列/资格过滤/配置；受控 pending 与时间验证 exact deadline、晚成功、重复事件、真实 fd 复用的旧 token、256 上界/取消、资源/注册错误。受控 timeout 不是自然网络超时。
- `v03_health_checker`：真实 TCP 接受和拒绝、probe 无 payload、清理；仅内部测试缩短间隔，生产参数固定。
- `v03_health_tcp_product` / `v03_health_udp_product`：标准库 Python fixture 启动原产品，默认 1s/3fail/2success，nonce 数据与空 probe 分别计数；Unknown、摘除/恢复、无 fallback、旧 TCP/UDP 绑定、热业务下维护与停止。
- 原 11 项回归保留；总 15 项，Debug 排除 udp_long 后 14，Release 15 含真实 60s expiry。Python3 仅测试依赖，Production `BUILD_TESTING=OFF` 不需要 Python。
- 原始日志、源码指纹及 Release 负向验证见 Builder/Reviewer 报告。未实现指标快照、应用层/UDP 主动探测、重试、迁移、热加载或 XDP。
