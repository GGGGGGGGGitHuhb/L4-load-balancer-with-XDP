# 配置规格

适用至 V0.3/S1：静态 TCP/UDP 配置格式。`--check-config` 不创建 socket、不启动监听、不连接后端，也不检查可达性；`--run` 按配置启动 TCP 或 UDP。

## 格式与字段

格式是项目限定的 `key=value` 行文本，不兼容通用 INI/TOML。

```text
# 可选字段；省略时为以下默认值
protocol=tcp
scheduler=round_robin
health_check=off
# TCP 端点
listen=0.0.0.0:8080
backend=127.0.0.1:9001
backend=127.0.0.1:9002
```

- `listen` 必须恰好一项；`backend` 必须为 1 至 256 项，保留文件顺序；没有默认端点。listen 可出现在 backend 之后。
- 键和值区分大小写，只接受 `listen`、`backend`、`protocol`、`scheduler` 和 `health_check`。重复 listen、重复 backend 端点、未知键、空键/值、缺少或多个等号失败。
- `protocol` 可选，缺省 `tcp`，仅接受 `tcp`/`udp`；`scheduler` 可选，缺省 `round_robin`，仅接受 `round_robin`。两个字段独立可省略，任意顺序，每个至多一次，即使重复同值也失败。未知值、大小写变体、空值均失败，首个错误行优先。
- 键和值外侧以及行首尾只去除 ASCII 空格和制表符；值内部禁止空白。
- 忽略空行和去除外侧空白后以 `#` 开头的整行注释；注释可含 UTF-8。不支持行尾注释、引号、转义、变量替换、include、节名。
- 接受 LF、CRLF、末行无换行；只移除 CRLF 的 CR，孤立 CR（包括最后一个字节）失败。
- 空文件、仅注释、NUL、UTF-8 BOM 失败；键和值禁止非 ASCII。NUL、BOM、孤立 CR 在注释中也失败。

## 端点

- 仅支持数字 IPv4 加冒号加十进制端口；禁止 DNS 和 IPv6。
- IPv4 恰好四段，各段 0..255；端口 1..65535。均禁止符号、前导零（单个 IPv4 段 `0` 允许）、尾随字符。
- listen 允许 `0.0.0.0`，backend 禁止；均禁止 224.0.0.0/4 多播和 255.255.255.255 广播。
- 其他数字地址只检查语法，不探测本机地址或网络可达性。

## 文件与错误

- 路径相对当前工作目录，不搜索默认文件，支持空格路径。以 `--` 开头的文件名请使用 `./` 前缀以避免与选项混淆。
- 文件必须是普通文件，允许指向普通文件的符号链接；目录、FIFO、设备失败。FIFO 使用非阻塞打开后检查类型，不等待写端。
- 最大 65536 字节，包含换行；逐块读取最多 65537 字节，在读取中检查上限，不依赖打开时大小。超过上限优先返回文件错误。
- 文件只读，不写回、不重试；解析失败不产生部分 Config。
- 文件错误包含路径；字段或语法错误报告首个失败行，重复字段报告后出现行。缺少必填字段在 EOF 报告（最后实际行之后的行号；空文件为 1）。诊断不回显配置内容。

## CLI

- `l4lb --help`：stdout 显示 help/check-config/run 用法和 TCP 固定轮询、UDP flow 尽力转发边界，退出 0，不读配置。
- `l4lb --check-config <path>`：成功 stdout 按协议为 `配置有效：TCP，后端数量=N` 或 `配置有效：UDP，后端数量=N`（末尾换行），stderr 为空，退出 0。
- 参数错误（无参数、未知参数、缺路径、重复选项、额外参数、help 混用）退出 2，stdout 为空，stderr 为用法错误和 help 提示。
- 文件或配置错误退出 1，stdout 为空，stderr 包含原因。

`health_check` 可选，缺省 `off`，仅接受 `off`/`tcp_connect`；任意顺序、至多一次，同值重复也报后出现行，大小写变体/空/未知值失败。启用后初始 Unknown 拒绝新会话/flow，连续2成功开放、3失败摘除，固定完成后1s/超时1s。UDP 必须提供相同 IP/端口且有代表性的 TCP 健康端点；TCP 握手不是 UDP 协议健康证明。详见 [健康规格](health-check.md)。目前不支持权重和热加载。

- V0.1/S2 新增 `l4lb --run <path>`：复用同一只读加载与错误规则，校验成功后启动 TCP 服务；与 check-config/help 互斥。详见 [TCP 转发语义](tcp-forwarding-semantics.md)，配置格式及默认值未变。

## V0.2/S2 兼容性与运行限制

- 旧配置和 `configs/example.conf` 不变，等价于显式 `protocol=tcp`、`scheduler=round_robin`，不会迁移或写回文件。
- 合法 UDP 配置现可运行；ready 为 `UDP 服务已启动：<配置地址>:<端口>` 加换行，端口冲突退出 1 且无 ready。静态校验仍不创建 socket。flow 容量、超时与数据报错误语义见 [UDP flow 规格](udp-flow-table.md)，没有新增配置字段。
- S1 历史版本仅允许 UDP 校验；回退 S1 会恢复 UDP 运行拒绝，不涉及配置写回或迁移。
- TCP 运行仍沿用已有日志、退出和关闭行为；控制层 TCP 直调入口也拒绝非 TCP 协议。
- 新键不保证能被 V0.1 读取。回退需移除新增键；不能将 UDP 配置当 TCP 降级使用。
- [调度规格](scheduler.md) 定义失败选择推进、实例隔离与静态池边界。
