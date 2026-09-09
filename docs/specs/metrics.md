# 指标规格（V0.3/S2，schema=1）

指标是本次run的内存累计量，默认关闭。`metrics=stderr` 与 `health_check` 独立；只有运行模式创建collector，静态配置检查不创建collector、socket或周期。重启全部归零，不做跨进程聚合。实现依据 Approved V0.3-S2-D1；验收状态见角色报告。

## 输出和字段

每条是 ASCII `metrics ` 前缀、完整JSON对象、换行；混在原stderr日志中，整个stderr并不是JSONL。字段固定按以下次序且全部出现；所有整数用无符号十进制，消费者须支持64位精确整数，不转浮点。

- `schema`：整数1。
- `seq`：uint64，从1递增；到上界饱和并重复同一上界，不能声称永远唯一。
- `phase`：`ready` / `periodic` / `final` / `error`。
- `protocol`：`tcp` / `udp`。
- `uptime_ms`：steady_clock相对本run指标创建时的毫秒，不是墙钟。
- `sessions_created_total`、`sessions_closed_total`、`sessions_active`：累计创建、累计关闭和当前活动数量。TCP是已进入会话表的client，包括connecting，不是“成功建立后端TCP连接数”；UDP是双索引和epoll事务提交的flow，不是TCP连接。正常未饱和时created−closed=active。
- `bytes_c2b_total`、`bytes_b2c_total`：成功提交给内核的payload字节。TCP每个send返回n>0立即计n，包括仍存活的会话；UDP仅完整成功(n==长度)计长度。不是对端收到、应用处理或链路字节；接收/排队/失败/probe不计，关闭日志sent数组不再次累加。
- `datagrams_c2b_total`、`datagrams_b2c_total`：UDP完整成功提交包数，零长也加1但bytes不加；TCP固定0。
- `rejected_total`：TCP已accept因容量/无健康资格拒绝的client；UDP有效新key因容量/无健康不能建flow的首包。已有flow不计，setup失败不属于策略拒绝。
- `dropped_datagrams_total`：UDP已取得的一包因截断、元数据无效、拒绝、setup失败、send失败/短提交/4次EINTR耗尽而未完整转发，各包一次，两方向合计。recv失败/异步错误没有已取得的包，不凭空加drop。TCP固定0。
- `errors_total`：下节的实际业务操作失败事件；不等于失败会话数，不依赖限频日志条数。
- `timeouts_total`：TCP connect/idle关闭及UDP idle到期，各一次；不是errors子项，正常超时清理也计。
- `counter_saturated`：布尔，累计计数或seq达到uint64最大值时true，不回绕；active是真实有界gauge，重复关闭返回模型错误，绝不钳零掩盖错误。
- `backends`：按配置顺序，最多256个 `{index,health,eligible}` 对象，内部字段顺序亦固定。index从0开始；health关闭时`disabled`且eligible=true，启用时`Unknown/Healthy/Unhealthy`，只有Healthy可选。采集只复制，不tick、不创建probe、不改变cursor。

rejected与drop可以描述同一个UDP包，不互斥。健康不eligible不禁止已有flow继续贡献bytes/datagrams。没有按backend流量账本或按client标签，不输出payload、client地址、配置路径。

## 真实事件落点和去重

数据面Callbacks的可选`statistics(StatEvent)`发送强类型事实，不使用测试Observation或解析reason字符串。control将事件交Collector；off不安装统计回调。

- TCP `accept_sessions`：会话表emplace成功Created；容量/空选择Rejected；ECONNABORTED等可恢复accept错误每次Error，资源错误在日志抑制前Error。EINTR/EAGAIN不计，传播的致命accept在control计。
- TCP `write_direction`：每次正send立刻BytesC2b/B2c；三个SessionFailure捕获点每次Error，随后close只Closed。`remove`非致命epoll DEL失败单独Error；`deadlines`超时先Timeout再close；正常drained/stop不Error。
- UDP `create`：只有事务成功后Created；容量/空选择Rejected；各setup事务失败一次Error，回滚不Closed。调用它的`listener_read`取得包但返回空token时统一Dropped；create/选择抛异常时同样先Dropped再传播，Error仍由control统一计，避免setup/reject/异常重复计数。
- UDP `send_packet`：完整成功分别Bytes与Datagram；短send为Error+Dropped；EAGAIN仅Dropped；其他实际非重试错误先Dropped再由`error`分类，4次EINTR耗尽只Dropped。
- UDP `error`：listener可恢复错误、backend压力错误均在diagnostic限频前Error，backend致命错误Error后erase；传播服务级错误不先计。EAGAIN/EWOULDBLOCK/EINTR不Error。`backend_read`/`listener_read` recv失败不Dropped；截断/坏元数据取得包仅Dropped。
- UDP `dispatch`：SO_ERROR读取失败或backend-hup按规定一次Error；异步错误由`error`处理，无包不drop；所有4个idle删除入口先Timeout。`erase`只Closed，不根据reason重复Error。
- control在reactor栈展开后，传播的服务异常统一Error一次并出error尾快照。启动失败如果collector已存在，可有error但绝无伪ready。底层已作为可恢复事务处理的错误不再传播为同一个顶层错误。
- 健康probe、状态转换、metrics输出失败和原日志失败均不属于业务errors；不声明覆盖内核未报告丢包或应用业务错误。

## 时序、资源和故障

原ready stdout先输出，随后ready快照全零，不等待探活；通常健康是Unknown。首次periodic在ready完成后1000ms；maintenance先维护健康、再检查指标，每次到期最多一条，完成后now+1000ms，不追赶积压；选择函数额外health tick不触发输出。

正常run返回时reactor已销毁，final恰好一条，active=0，closed包括停止清理。异常也先清理，再error恰好一条，传播原服务异常。SIGKILL/崩溃或进程级信号终止不保证尾快照。原ready/check/退出文本保持。

Collector固定大小、无更新分配或异常；Snapshot按值复制，backend固定数组256；格式化单条上限32768字节，完整缓冲后单次write提交。没有每I/O输出、每client map、无限队列或写线程。

**同步stderr可能被慢文件、终端或堵塞管道拖慢数据面和停止**；管道关闭可依POSIX SIGPIPE终止进程。建议普通本地文件并由外部管理轮转。本阶段不保证非阻塞、可靠落盘或跨进程行原子性，不修改全局O_NONBLOCK。

已经返回的write失败、短写或格式化异常使本run后续metrics输出禁用，无重试、不清全局stream错误状态、不向同一sink递归报错；不会把输出错误改算业务错误或覆盖原服务异常。底层失败可留半行，不自动修复；消费者只接受换行完整JSON。

## 解析和验证

下面示例保留Python精确整数，仅接受完整metrics行；业务日志另行处理：

```python
import json
with open("service.stderr", encoding="utf-8") as source:
    for line in source:
        if line.startswith("metrics ") and line.endswith("\n"):
            try:
                snapshot = json.loads(line[8:])
            except json.JSONDecodeError:
                continue  # 故障可能留下损坏行
            if snapshot.get("schema") == 1:
                print(snapshot["sessions_active"], snapshot["bytes_c2b_total"])
```

- `v03_metrics_unit`：严格配置组合/错误、模型每字段、饱和/隔离/重启/不变量、受控周期/seq/格式化、writer失败/短写及真实`/dev/full`。
- `v03_metrics_tcp_events` / `v03_metrics_udp_events`：同一reactor代码实际落点，真实TCP部分send与受控UDP syscall结果，验证限频前错误、零长、短发/EAGAIN/EINTR/recv无drop/setup/timeout/清理。注入不宣称自然网络故障。
- `v03_metrics_tcp_product` / `v03_metrics_udp_product`：原CLI、普通stderr文件、严格JSON字段/顺序/类型；独立nonce验证活跃periodic、半关闭/停止、健康probe排除、旧flow不eligible仍传、新key的Rejected+Dropped及启动error。
- 原15项保留，总20项，Debug快速19、Release完整20含一次原60s expiry；Production不构建测试。三类Release负向、PID/fd审计、日志和源码指纹见Builder/Reviewer报告。没有S3完整故障矩阵、速率/延迟、Prometheus或XDP。
