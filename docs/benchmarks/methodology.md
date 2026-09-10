# 用户态 TCP/UDP Benchmark 方法（V0.4/S1）

本工具验证测量方法与证据可靠性，不代表 V0.4 优化完成。仅使用 Python3 标准库，产品独立构建不依赖 Python。默认在 Linux/WSL loopback 执行，无外网、root、第三方服务或调参要求。WSL、共享 CPU、Python 生成器与 echo 后端均可能成为瓶颈，结果不能推断物理网卡、云服务器或 native XDP 上限。

## 可复制构建与测量

在仓库根目录执行；输出目录必须不存在，失败目录保留，重试使用新名称。以下 B 可替换为仓库内任意新目录：

```bash
B="$PWD/.stage-tmp/benchmark-example"
mkdir -p "$B/tmp" "$B/cache" "$B/pycache"
export TMPDIR="$B/tmp" TMP="$B/tmp" TEMP="$B/tmp"
export XDG_CACHE_HOME="$B/cache" PYTHONPYCACHEPREFIX="$B/pycache"
cmake -S . -B "$B/Production" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$B/Production" -j4
python3 tests/benchmark_runner.py --program "$B/Production/bin/l4lb" --protocol tcp --mode paired --output "$B/tcp-default"
python3 tests/benchmark_runner.py --program "$B/Production/bin/l4lb" --protocol udp --mode paired --output "$B/udp-default"
```

普通 Linux 可直接执行。环境若阻止 socket 或 WSL loopback 大包回归，沿用项目隔离路线，在 `unshare --user --map-root-user --net bash` 内先执行 `ip link set lo up` 再运行网络测试；只设置新 namespace 的 lo，不修改宿主网络。该边界在受限执行器中需要窄范围授权。

## 固定负载与边界

- `--protocol tcp|udp`；`--mode direct|proxy|paired`，默认 paired。direct 一个 echo 后端；proxy 一个真实 l4lb 加同类后端，健康检查与 metrics 都 off。每个子 run 新建进程、配置、socket 与 nonce。
- 默认 `--payload 256 --clients 1 --warmup 1 --duration 5 --rate 1000 --timeout 1 --repeats 3`。payload 包含 32 字节身份头：16 字节 nonce、4 字节 phase、4 字节 client、8 字节 seq。
- 参数保护：clients 1..64、payload 64..4096、warmup 0..10秒、duration 1..60秒、rate 1..100000 pps、timeout 0.1..5秒、repeats 1..10。NaN/无穷/越界拒绝；不保证机器达到上界。
- 每协议默认 paired 3次，共6个run，顺序 direct/proxy、proxy/direct、direct/proxy。clients16 验证追加 `--clients 16 --repeats 1`，其余默认不变。性能矩阵串行运行，不取最佳值替代其他 run。
- TCP 每 client 在测量前建立一条长连接，完整校验一个定长回显才发送下条；实现 partial send/recv，RTT 从第一次发送尝试前至完整校验后。预热使用独立 phase，等待其全部回显后开始测量；正式窗口内不新建连接。这是闭环吞吐，不是短连接能力或链路带宽极限。
- UDP 总目标 rate 按 round-robin 槽均分到独立固定源 socket，不随 clients 翻倍。单调时钟节拍，错过槽直接计 missed_slots，不突发补发。pending 上限65536，序号状态与原始client归属各用最长600万槽的bytearray（每槽各1字节），达到pending上限也跳槽。回显先检查seq属于原发送client，再进行成功、重复、迟到分类或超时状态变化；pending移除后仍保留归属。UDP未确认允许出现，不自动当工具失败。
- 预热计数单独保存，旧phase响应明确排除。正式窗口为 `[t0,t1)`；之后仅对已发送包等待至其 timeout；无新请求。TCP drain 等最后一次交换，UDP drain 等未决包收到或超时。

## 统计与解释

- received 仅完整校验的唯一有效回显，吞吐 payload 只计一次，不把请求和回显相加。`goodput_bytes_per_second = received * payload / goodput_elapsed_seconds`；bit/s 精确8倍。
- `send_window_seconds` 是 t1-t0；`goodput_elapsed_seconds` 是 t0 至有界 drain 完成的真实时间。`sent_per_second` 用发送窗口作分母，goodput 用包含drain的时间，不混称。
- UDP保存 target（目标槽数）、attempted、sent、received、timeout、duplicate、late、foreign_corrupt、send_errors、missed_slots、pending_peak、实际发送率和 under_target。delivery_ratio=received/sent，loss_ratio=(sent-received)/sent。sent=0时ratio为null且run无效；没有成功回显也无效。
- 超时后的UDP回显计late而非成功；已经成功的回显再次出现计duplicate。无法通过nonce、phase、client、seq及全payload校验会使测量非0退出。有效低负载 smoke 显式要求零超时；普遍压测不把丢失或under_target等价于工具失败。
- RTT为 **sampled RTT**：仅seq为100倍数的成功记录保留原始ns/client/seq，吞吐计数全量。nearest-rank为 `ceil(p*N)`、1-based；空样本null。报告stride、count、coverage，样本不足100条标记p99不足，不能称全量百分位；采样为确定性周期，可能受周期性工作负载偏差影响。
- 成功RTT不包含丢失请求，必须和loss/timeout并列解释。汇总按mode给各run列表及goodput中位/最小/最大，不跨run混合RTT。direct/proxy差值不是已隔离的纯代理开销。
- 产品、生成器、后端分别读取各PID `/proc/PID/stat` 的 user+system ticks；使用各自末次减首次CPU秒/相同实际墙钟，单逻辑核=100%。direct产品为null/not applicable。资源窗口包住正式流量及drain，实际边界在资源记录中，不用生命周期rusage代替。
- 每50ms采样RSS与fd，并在进程退出前末次采样；报告sampled peak与实际样本数，可能漏掉瞬时峰值。必需/proc字段读取失败判无效，原始资源行保留；可选环境字段失败为null+reason，不填0。

## 证据格式与生命周期

每个输出根有 `summary.json`；每次重复/mode有独立目录：

- `environment.json`：参数/命令、Git commit与dirty status、产品SHA256、工具与生产源码指纹、构建缓存/编译器/CMake、内核/WSL/CPU/亲和性/内存/fd/cgroup与namespace。只取环境白名单，不导出凭据或环境变量全集。
- `configuration.txt`、proxy的`config-N.conf`、后端ready记录、产品/后端stdout/stderr日志。
- `result.json`：schema_version=1、valid、UTC开始/结束、单调窗口、计数、单位、采样、资源、主因/清理次因、退出码、owned PID回收、fixture fd前后。正式result路径失败时写 `failure.json` 保留原因。
- `rtt-samples.jsonl` 与 `resource-samples.json`：可独立重算的原始样本。启动/流量失败可能没有完整RTT，不能伪造缺失值。

valid仅表示测量及证据完成，不表示速度优秀或达到target。后台异常、产品意外退出、损坏、必需资源失败、写失败和清理失败均非0退出。先保留primary error，再独立列cleanup errors。内部硬超时记录在result，启动ready最多5秒；端口冲突最多3次并保留日志；TERM等待2秒后KILL/reap，只管理自己创建的PID。中断run保持无效，不覆盖旧证据。

## 短验证与可选完整工具验收

```bash
# 测试构建下的短组，3项，长性能矩阵不注册到默认CTest
ctest --test-dir build-release -L v04_bench_smoke --repeat until-fail:2 --output-on-failure
# Production公开工具验证矩阵：默认两协议各3对 + clients16各1对
python3 tests/benchmark_acceptance.py --program "$B/Production/bin/l4lb" --suite matrix --output "$B/matrix"
python3 tests/benchmark_acceptance.py --program "$B/Production/bin/l4lb" --suite failure --output "$B/failure"
python3 tests/benchmark_acceptance.py --program "$B/Production/bin/l4lb" --suite udp-check --output "$B/udp-check"
python3 tests/benchmark_acceptance.py --program "$B/Production/bin/l4lb" --suite isolation --output "$B/isolation"
# 当前普通uid单独运行；namespace root不算权限证据
python3 tests/benchmark_acceptance.py --program "$B/Production/bin/l4lb" --suite permission --output "$B/permission"
```

负向工具使用隐藏 `--fault` 测试接口，实际走正式接收、子进程监测、资源、写入与清理路径；属于工具注入，不伪称真实产品故障。`failure`另用7字节TCP回显碎片和UDP确定性乱序/重复/延迟fixture验证解析。`udp-check`单独运行UDP损坏、跨client身份改写并交换目的socket的真实proxy负向，以及合法乱序/重复/迟到正向；这些检查也包含在failure组。跨client必须明确返回client ownership mismatch、非0且完整回收，不能把错误回显计成功。isolation的两组并行与带空格路径只验证隔离，不作性能比较。不可写测试在流量已发生后改变本run目录权限，并在finally恢复可写以保存主因和回收证据。

失败先看 summary主因，再看子目录failure/result、进程stderr与资源记录。不要改生产配置、关闭宿主服务、反复覆盖失败目录或用外层timeout124代替检出。报告使用[模板](report-template.md)，样本见[工具验证样本](tool-validation-sample.md)。

## V0.4/S3 固定跨版本比较

S1 单次 runner 入口兼容。跨版本正式报告使用 `tests/v04_benchmark_compare.py`，其 `product_identity.json` 关联经校验构建 manifest；旧 environment 的 git_commit / production_source_sha256 始终是**工具工作树上下文**，不得据此标注旧产品来源。固定 baseline 为 `v0.4-s1` / `8a1b9393e8a1710152274be525c015bb2623625f`，candidate 为 `v0.4-s2` / `48a12831b2d0a767fd5d4bb1b2899d2f078be2ac`。来源从只读 Git blob 导出，不切换工作树；两方以相同 clang++、Release、BUILD_TESTING=OFF、Ninja 与 flags 构建，manifest 记录 Git tree/tag 对象、源码 SHA、argv/退出码、工具版本、实际缓存 flags 和 binary SHA；测量前逐项核验。

在仓库根执行，B 必须是新的输出位置。无网络下载；构建普通 uid 执行，测量在临时 user/net namespace 只启用 lo（当前环境需要升级权限时使用窄命令批准）。不修改宿主网络或亲和性。

```sh
B="$PWD/.stage-tmp/local-compare"
mkdir -p "$B/tmp" "$B/cache" "$B/pycache"
export TMPDIR="$B/tmp" TMP="$B/tmp" TEMP="$B/tmp"
export XDG_CACHE_HOME="$B/cache" PYTHONPYCACHEPREFIX="$B/pycache"
python3 tests/v04_benchmark_compare.py build --output "$B/products"
unshare --user --map-root-user --net sh -c 'ip link set lo up && exec python3 tests/v04_benchmark_compare.py run --builds "$1/products" --output "$1/formal"' sh "$B"
python3 tests/v04_benchmark_compare.py recompute --package "$B/formal/raw.json.gz" --output "$B/recomputed"
# 普通 clone 从已跟踪公开包离线重算，无需本机产品/build/角色目录
python3 tests/v04_benchmark_compare.py recompute --package docs/benchmarks/reports/v0.4-user-space.raw.json.gz --output "$B/published-recomputed"
```

固定矩阵 TCP4096/UDP256，clients1/16，warmup1s、duration5s、timeout1s、stride100，UDP 总目标10000pps；每组合三轮、每轮两版本各 direct/proxy 共48run，全部串行。第0/2轮 baseline→candidate、direct→proxy；第1轮相反。禁止测量期间构建/回归/另一批压测。每run新 nonce、子进程和目录；direct 关联版本但产品CPU为 null。`--smoke` 仅四个1秒TCP run，不可形成正式报告。

公开包为 gzip JSON，单次读取上限128 MiB，无解包路径写入。保存完整计数、RTT与资源行、参数、manifest、工具SHA、环境、顺序和清理结果；包内整体与逐run SHA 检测损坏。路径前缀仅替换为 `$REPO`，manifest摘要随相对化重算；不改变任何数值。原始进程日志/缓存保留在本地证据根，不入Git。离线汇总核验完整矩阵及身份，再从原始行重算 nearest-rank RTT、单核CPU、sampled peak RSS/fd、goodput/loss；不能用改包SHA掩盖语义错配。

汇总保存16组各三次值及中位/最小/最大，以及同轮 proxy candidate-baseline 差和百分比；0分母百分比null并说明。三次数据不做显著性推断，不混池RTT，不机械扣direct后称纯代理开销。丢失、未达目标、missed slots和p99样本不足均保留。S1→S2多项变化不能全部归因给ring。

短自测：`python3 tests/v04_benchmark_compare_test.py`；CTest标签 `v04_compare`，正式48run不注册默认测试。可在同一隔离网络命名空间执行 `python3 tests/v04_benchmark_compare_test.py acceptance --builds "$B/products" --output "$B/validation" --package "$B/formal/raw.json.gz"`：短并行两组、空格路径、错binary拒绝、真实启动后失败及计数/RTT/格点/身份负向。输出必须新建，失败目录保留；并行数据不进入正式报告。
