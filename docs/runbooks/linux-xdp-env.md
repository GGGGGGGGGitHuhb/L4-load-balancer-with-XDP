# 本地 Linux/WSL2 XDP 环境

2026-09-11：采用本地环境，不要求购买云服务器。依据用户“如果本地都可以跑，那就直接本地”授权，以及本页范围内的环境能力验证。此决定替代 ROADMAP、ARCHITECTURE、TD-003 中原先强制云端运行的要求，保留历史报告。

## 9月11日预检环境与证据来源

- 用户终端：Ubuntu 24.04.3 LTS，Linux 6.6.87.2-microsoft-standard-WSL2 x86_64。
- 工具：Clang 18.1.3、LLVM 18、iproute2 6.1.0/libbpf 1.3.0；Python 3 标准库通过 ctypes 调用系统 libbpf.so.1。未安装 bpftool，探针不依赖它。
- BPF 编译使用 `-target bpf -O2 -g -isystem /usr/include/x86_64-linux-gnu`。此头文件路径限定本次 x86_64 环境；正式构建须处理目标平台。
- 用户从普通 UID 调用 sudo 执行；普通用户加载拒绝另测。Agent 未获得免密码 sudo，动态测试由用户终端运行，Agent 核对磁盘 result-02.log 与回传结果，不能冒充独立 Reviewer 执行。
- [机器摘要及文件指纹](local-xdp-preflight.json)包含内核、两模式结果、脚本/对象/两次日志 SHA256。完整探针和原始日志存于私有治理证据 `docs/leader/reports/V1.1/environment-evidence-001/`，不随普通 clone 分发；正式公开可复现工具属于后续阶段产出，当前摘要不声称包含完整复现包。

## 实际测试及结果

拓扑：一个临时 network namespace 中建立 xa—xb、xc—xd 两对 veth。xa 注入构造的固定 IPv4/UDP 测试帧，xb 挂载探针；放行时本命名空间 UDP socket 接收精确载荷，重定向时从 xc 发出并在 xd 接收端检查精确帧。原始帧发送避免将同命名空间 IP 本地路由误当作经过 XDP 的流量。

- generic：12项 PASS；native veth：同样12项 PASS。
- 加载、挂载、重复挂载拒绝、不存在接口拒绝。
- UDP放行、丢弃时不送达、map切换后恢复。
- map更新/读取一致、每个测试包计数精确递增。
- map指定出口的 XDP_REDIRECT，目标peer接收帧与发送帧完全一致。
- 卸载、卸载后UDP正常恢复。
- 普通UID加载被拒绝，属于预期负向。日志中的权限不足、重复挂载错误不是测试失败。

以上为24个模式检查及普通UID权限检查。未记录各测试耗时或性能数据。

## 生命周期与修正记录

脚本使用有界 timeout 和 `unshare --net`。每模式 finally 关闭socket和BPF对象、卸载本次程序；未显式清理成功的挂载由namespace退出兜底。未创建持久pin或命名netns，不修改宿主接口、路由、代理或sysctl。当前通过日志明确证明xb卸载和UDP恢复；namespace销毁依赖进程退出机制，没有额外全局BPF枚举证据。

首次运行 result.log 在读取继承的 `/sys/class/net/xb/address` 时失败，尚未进入流量测试；这是探针错误。修正为通过 `ip -json link` 的netlink查询获取当前namespace的接口地址，result-02.log两模式通过。失败记录保留。

## 阶段验收边界

以下列表只保留9月11日预检时的阶段边界。S1/S2目前已经交付项目对象和loader，见文末当前进展；不能把本段历史要求理解为当前未实现，也不能把预检回写为当时已经交付产品。

- V1.1：基础环境满足推进条件；正式BPF构建、项目loader、设备参数/错误提示、测试分层及独立验收仍需实现。
- V1.2：已证实map/统计、固定IPv4 UDP包判别和出口重定向所需基础能力；项目后端schema、控制面同步、受限转发协议/异常边界、用户态回归仍待对应阶段验收。本探针不是负载均衡产品。
- native veth仅代表虚拟网卡驱动模式，不代表本机Wi-Fi、物理网卡native或硬件offload。
- 性能对比仍必须做：记录主机、WSL资源、内核、模式、拓扑、流量发生器、重复次数与原始数据，控制共享CPU和虚拟化干扰。当前无性能结论，后续结果限定本地实测环境。
- 更新内核、libbpf或拓扑后重新预检；若后续实际功能失败，先诊断具体限制，不能以本次PASS豁免，也不自动恢复云采购要求。

## 2026-09-14 当前产品验证进展

S1独立BPF对象与S2项目loader已交付，S2已合并至main（5284777，PR17），v1.1-s2标签已推送。实际产品范围见[构建](xdp-build.md)及[加载与卸载](xdp-loader.md)，当前公开可复现流程见[V1.1最小验证](xdp-validation.md)。

S2与S3使用项目loader及公开测试脚本，不再依赖9月11日ctypes环境探针。Agent通过WSL官方root入口启动脚本自建net namespace，Builder和Reviewer各用独立构建产物；该执行来源与上文用户手动sudo的旧预检不同。两mode真实验证包含ID、UDP通流量、条件卸载/替换保护和生命周期清理。最新阶段结果见[验收索引](../specs/v1.1-acceptance.md)与[机器摘要](xdp-validation-result.json)。

libbpf开发文件来自Ubuntu官方1.3.0包；本轮只解包到临时构建依赖目录，不修改系统安装。正常复现可安装libbpf-dev。当前没有map业务、负载均衡fast path、物理网卡或性能结论；map文档为[初稿](../specs/xdp-map-schema.md)。内核/权限/驱动或拓扑变更后应重新验证。

## 参考

- [libbpf API](https://libbpf.readthedocs.io/en/latest/api.html)
- [Linux XDP redirect](https://www.kernel.org/doc/html/next/bpf/redirect.html)
