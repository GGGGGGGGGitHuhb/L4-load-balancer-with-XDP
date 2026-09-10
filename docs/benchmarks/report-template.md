# Benchmark 报告模板

## 目的与有效性

- 日期、操作者、工具版本/源码指纹、产品commit/dirty及二进制SHA256：
- 目的：工具验证 / 特定优化对照（说明），不直接声称链路或XDP极限。
- 所有run证据路径、退出码、valid与失败原因；无效run保留并解释：

## 环境与参数

- 内核/WSL、CPU/核数/亲和性、内存、fd与cgroup限制，未知字段及原因：
- 编译器/CMake/构建类型、拓扑/namespace/地址、配置、完整命令：
- 协议、payload含身份头、clients、warmup、duration、总UDP rate、timeout、repeats：
- direct/proxy一个同类backend、health/metrics off；实际执行顺序：

## 每次运行（逐run填写，不挑最佳值）

| Run / mode | valid | send秒 / 含drain秒 | target / attempted / sent / unique received | missed / timeout / duplicate / late / corrupt / send error | bytes/s / bit/s | loss / under-target |
| --- | --- | --- | --- | --- | --- | --- |
| 待填 | | | | | | |

- 每run sampled RTT：stride、count、coverage、nearest-rank p50/p95/p99/max(ns)，是否不足100样本；并列loss，绝不称全量RTT。
- 产品/生成器/后端分别：PID、CPU秒差/实际墙钟/单核100% CPU、sampled peak RSS/fd、采样起止与间隔；direct产品null。
- owned PID回收与返回码、生成器/后端fd前后、primary/cleanup errors：

## 配对与证据局限

- 各mode goodput中位/最小/最大及所有run路径；不混合跨run RTT：
- direct/proxy参数一致性，顺序交替，差值可能包含调度与工具自身负载：
- 瓶颈候选及支持证据（产品CPU、生成器CPU、后端CPU、missed slots等）：
- WSL/loopback/共享CPU/Python生成器、确定性采样偏差、RSS漏峰局限：
- 结论、未解决问题、下一步；不预填性能数字或宣称V0.4/S2/S3完成。

## 跨版本正式报告补充

- 固定 ref/commit/tree、tag 对象、产品 build manifest SHA 与 binary SHA；相同工具SHA。environment 的工具工作树身份与 product_identity 的被测身份分列。
- 同设置/编译器构建命令及实际flags、明确串行schedule和48格点逐run索引；失败尝试另列，不拼接批次。
- 每版本/协议/clients/mode三值+中位/最小/最大；同轮proxy绝对/相对差，0分母null原因；丢包与RTT并列，RTT样本<100的p99标不足。
- 可跟踪公开原始包及SHA、无本机依赖的重算命令；CPU/RSS/goodput均可从原始数据复核。Builder正式批次与Reviewer独立复测分开。
- 将事实、瓶颈候选、未验证解释分开；允许本矩阵未确定瓶颈或收益，不能用结构变化单因解释全部差值。
