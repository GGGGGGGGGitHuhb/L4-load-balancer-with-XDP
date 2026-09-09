# V0.4/S1 工具验证样本

2026-09-09；WSL2 loopback，Production Release；只验证工具，不表示优化成果或硬件极限。

- **历史TCP（Builder001）**：复用F-001修复前默认3对/clients16一对，共8run。UDP归属修复不改变TCP；这些样本没有伪称修复后重跑。
- **修复后UDP（Builder002）**：重新生成默认3对/clients16一对，共8run；seq在pending、duplicate、late状态均核对原client归属。
- 每组独立tool_sha256/身份，及每run明确evidence_generation，见[结构化样本](tool-validation-sample.json)。原UDP和首次失败复现保留在本地Builder/Reviewer证据目录，不覆盖历史。

产品SHA256：`acecac75404857f484ace2170adfa6d04c451fa92b71165e3265c78eb7c33c7c`，基线`281db01`且工作区dirty如实记录；生产二进制没有改动。完整原始RTT/资源/配置/日志在各run的仓库内path，批量构建/日志未提交。用[公开方法](methodology.md)重建自己的证据。

| 证据代次 / 协议 / clients / repeat / mode | sent / received | bytes/s | loss | sampled RTT数 / p99(ns) |
| --- | --- | --- | --- | --- |
| 历史TCP / tcp / 16 / 0 / direct | 21956 / 21956 | 1123672.22 | 0.0000% | 224 / 9037912 |
| 历史TCP / tcp / 16 / 0 / proxy | 20534 / 20534 | 1050472.03 | 0.0000% | 210 / 8513926 |
| 历史TCP / tcp / 1 / 0 / direct | 60507 / 60507 | 3097808.71 | 0.0000% | 606 / 252727 |
| 历史TCP / tcp / 1 / 0 / proxy | 30202 / 30202 | 1546293.48 | 0.0000% | 303 / 532148 |
| 历史TCP / tcp / 1 / 1 / proxy | 33019 / 33019 | 1690519.13 | 0.0000% | 331 / 357396 |
| 历史TCP / tcp / 1 / 1 / direct | 59866 / 59866 | 3065048.90 | 0.0000% | 599 / 247051 |
| 历史TCP / tcp / 1 / 2 / direct | 60570 / 60570 | 3101092.40 | 0.0000% | 606 / 227024 |
| 历史TCP / tcp / 1 / 2 / proxy | 29916 / 29916 | 1531623.88 | 0.0000% | 300 / 391247 |
| 修复后UDP / udp / 1 / 0 / direct | 4999 / 4999 | 255945.13 | 0.0000% | 50 / 386732 |
| 修复后UDP / udp / 1 / 0 / proxy | 4997 / 4997 | 255842.65 | 0.0000% | 50 / 680435 |
| 修复后UDP / udp / 1 / 1 / proxy | 5000 / 5000 | 255993.65 | 0.0000% | 50 / 577363 |
| 修复后UDP / udp / 1 / 1 / direct | 4998 / 4998 | 255891.16 | 0.0000% | 50 / 627762 |
| 修复后UDP / udp / 1 / 2 / direct | 5000 / 5000 | 255995.60 | 0.0000% | 50 / 401944 |
| 修复后UDP / udp / 1 / 2 / proxy | 5000 / 5000 | 255990.43 | 0.0000% | 50 / 539386 |
| 修复后UDP / udp / 16 / 0 / direct | 5000 / 5000 | 255995.62 | 0.0000% | 50 / 395410 |
| 修复后UDP / udp / 16 / 0 / proxy | 4991 / 4991 | 255521.54 | 0.0000% | 50 / 615641 |

所有列出的run都有成功传输。吞吐全量，RTT每100序号采成功记录，低于100样本的p99不足标志保留于JSON；低RTT必须并列loss/missed_slots/under_target。产品/生成器/后端分别保存窗口CPU与sampled peak RSS/fd，direct产品为null。

WSL调度、共享CPU、Python生成器及同机后端限制保持；direct/proxy差值不是纯代理开销，更不是S2优化或S3正式报告。首轮Reviewer001的F-001失败及Builder002修复记录保持，Reviewer002已独立复审PASS，F-001 Closed，Leader003已完成S1收尾；上述两代样本身份保持原样。修复后重算/PID/fd证据见 `.stage-tmp/v0.4-s1/builder/rework/evidence/audit.json`。
