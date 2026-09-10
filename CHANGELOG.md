# Changelog

### 2026-09-10 阶段后格式维护与提交hook

- 按现有Google / SeparateDefinitionBlocks规则检查全部98个追踪文件，44个C/C++中仅9个头文件增加31个分隔空行；非空白token、注释正文、字面量和预处理指令保持，原.clang-format未改。历史benchmark报告/原始包保留原字节与当时指纹。
- 新增.githooks/pre-commit，提交前格式化全部追踪C/C++工作树并独立检查暂存blob；需要重新暂存时阻止提交，绝不自动git add，保护部分暂存。README给出启用和处理步骤。
- 临时仓库12次真实commit覆盖正常/拒绝、部分暂存、空格/换行/删除/未追踪/符号链接/缺工具等路径；Release构建及6项短单元/产品smoke通过，不重跑或改写S3性能结果。

### 2026-09-10 V0.4/S3及V0.4开发范围完成

- D1保持Approved，Builder001、Reviewer001独立PASS及Leader003收尾齐备，S3六AC与V0.4六标准全部完成。正式S1/S2报告、公开原始包和离线重算入口交付，数据保持Builder最终48run，Reviewer独立48run不混入报告。
- 独立Debug28/28、Release29/29、Production/uid38/F-001/11短调用通过；两套48run分别独立原始数学审计，公开全部表格一致，PID/fd回收完整。
- 同步阶段/版本矩阵/报告验收及TD-004正式用户态报告义务，XDP义务保留；无新增发现、返工或债。历史工具元数据修复批次保留，无普遍性能提升承诺；本阶段尚未提交、合并或发布，由父按授权本地提交。

### 2026-09-10 V0.4/S3独立验收PASS

- Reviewer (Athena) 独立导出并构建固定S1/S2，完成串行48run、最终Debug28/28、Release29/29、Production、普通uid38例、短重复/并行/空格/故障回收和F-001；没有生产代码修改。
- 不调用被测聚合函数，分别从公开Builder包和独立Reviewer包重算48run原始统计、16组全指标及12组差值，公开Markdown全部表格一致；每批48runner/72owned PID回收，18短run的18runner/27owned PID回收。
- Reviewer001 PASS，无新增发现或技术债，当前Closing；交Leader同步六标准和最终状态，尚未提交、推送或发布。两套数据分别保留，未宣称普遍性能提升或提前完成XDP/V1.0。

### 2026-09-10 V0.4/S3实现与自测完成

- 交付固定S1/S2构建manifest及同工具串行48run对照、独立原始重算、公开紧凑数据包/报告/六标准矩阵。来源/binary/工具身份分列，失败run启动前身份和主因/cleanup独立保留。
- Debug28/28、Release29/29（原expiry一次）、Production/普通uid38、短重复/并行/空格及真实失败/数据负向通过；最终48run与全部分组重算、PID/fd回收一致。保留首个完整批次及证据补丁过程，最终发布新整批，不拼接。
- Builder001 Ready for Review，待独立Reviewer和Leader收尾；生产src/configs零diff，无性能提升门槛，未提交/推送或发布，未提前完成V0.4或关闭XDP义务。

### 2026-09-10 V0.4/S3批准登记

- PM批准V0.4-S3-D1决策包，设计和审查方案Approved / Ready for Builder，新增Leader002。统一工具与产品身份、48run串行矩阵、公开可重算数据和六标准验收范围不变。
- 授权实现、测量、自测、独立审查和Leader收尾，完成后本地提交；尚未实施或验收，不含push或发布，不提前完成V0.4。

### 2026-09-10 V0.4/S2标签核验与S3准备

- 远端main与v0.4-s2注释标签peeled核验48a1283，S2已合并PR11并发布；同步本地main，创建codex/v0.4-s3。
- 形成V0.4-S3-D1 Draft、审查方案及准备报告，定义固定版本身份、48run公平比较、原始数据重算与六条验收要求。未实现、未测量、未提交；历史阶段及发布记录保持原时点事实。

### 2026-09-10 V0.4/S2完成

- D1保持Approved，Builder001、Reviewer001独立PASS和Leader003收尾齐备，S2 Completed；固定环形缓冲、有界TCP待发队列停止和异常安全清理已交付。
- 最终源码独立Debug27/27、Release28/28（含expiry）、Sanitizer5/5、普通uid38及Production/重复/并行/空格/工具负向通过；43产品run的43PID、4兼容run的6PID回收，fd一致。额外随机nonce双向pending在ASan/UBSan通过。
- 同步阶段、README、ROADMAP及技术债状态；无未解决发现或新增债项，Builder日志行屏障首次失败与修复证据保留。S3正式性能报告和V0.4整体尚未完成，不宣称吞吐提升；本阶段尚未提交、合并或发布，父按授权本地提交。

### 2026-09-10 V0.4/S2独立验收PASS

- Reviewer (Athena) 对最终源码独立 Debug 27/27、Release 28/28、Production、Sanitizer 五项、普通 uid 38 例及新增专项两次重复验收通过；另用随机字节加强双向 pending 排空验证。
- Production 五场景、并行/空格/目标负向、TCP/UDP 短 paired 与 F-001 通过；独立审计 43 产品 run（含 3 预期负向）及 4 兼容 run 的 PID/fd 回收。无新增发现或技术债，详见 Reviewer001。
- 当前 Closing，交 Leader 同步最终阶段状态；未提交、推送或发布，不提前完成 S3 或宣称性能提升。Builder 首次工具竞态失败记录保留。

### 2026-09-10 V0.4/S2实现与自测完成

- TCP 固定环形缓冲与连续 span 转发、有界 1 秒用户态 pending 排空和第二信号强制停止；TCP/UDP 先释放资源再独立通知，析构回收多 owner 并保留异常边界。同步规格、停机文案与公开生命周期验证 runbook。
- Debug 27/27、Release 28/28、Production、Sanitizer 五项、普通 uid 权限通过；最后停止守卫修正后完成受影响状态/Sanitizer/产品补测。保留并修复产品工具读取未完成日志行的首次并行失败；最终 23 个产品正负场景与 4 个短兼容 run 的 PID/fd 审计通过。
- Builder001 Ready for Review，尚待独立 Reviewer 与 Leader Closing；未提交、推送或发布，不宣称性能提升，S3 未开始。

### 2026-09-10 V0.4/S2批准登记

- PM批准V0.4-S2-D1决策包，含TCP最多1秒尝试排空已进入用户态队列的停止行为、环形缓冲及异常安全关闭；设计和审查方案转Approved / Ready for Builder，新增Leader002。
- 范围与七条AC保持，授权实现、自测、独立验收和收尾，完成后本地提交；尚未实施或验收，不含推送或发布，S3正式性能报告仍留后续。

### 2026-09-09 V0.4/S1标签核验与S2准备

- 已核验远端main与v0.4-s1注释标签peeled一致为8a1b939，包含S1验收提交8143236；同步本地main并创建codex/v0.4-s2。
- 形成V0.4-S2-D1 Draft设计、审查方案与准备报告，明确环形缓冲、有界TCP待发队列停机、异常安全清理及七条验收要求。未改变产品行为、未运行测试、未批准实现；历史完成/发布记录保留原时点事实。

### 2026-09-09 V0.4/S1完成

- Leader003核对D1批准、Builder002与Reviewer002 PASS并完成收尾，S1标记Completed，D1保持Approved；F-001已修复关闭，首轮FAIL及原始证据保留。
- 交付TCP/UDP直连与代理benchmark工具、测量方法、报告模板和分代工具验证样本。独立Debug25/25、Release26/26、Production及修复后UDP八run、正式负向、短组两次、隔离通过；复审19run独立重算无不一致、30个owned PID回收。未受影响路径保留首轮验证，未冒称复审重跑全套。
- 同步README、ROADMAP、阶段权威及TD-004已完成的方法/格式义务；S2生产优化、S3正式性能报告和XDP均未完成，不宣称V0.4整体完成或性能提升。父将按授权本地提交，尚无本阶段合并或发布。

### 2026-09-09 V0.4/S1独立复审通过

- Reviewer002：PASS，F-001（P2）Closed；保留Reviewer001首轮FAIL及原复现。原三层独立跨client试验均确认正确拒绝，pending/duplicate/late三态在统计或状态变化前核对原始client归属。
- 独立UDP默认/clients16八run、UDP损坏/跨client负向、合法乱序迟到、短组连续两次及隔离通过；新19run（16有效、3预期无效）独立重算一致，30个owned PID回收、fd相等。首轮未受影响生产构建/回归证据明确复用。
- 公开样本历史TCP八run与修复UDP八run分组/参数/指纹一致，未伪称旧样本重跑；当前Closing，待Leader同步后才能标Completed。

### 2026-09-09 V0.4/S1 F-001修复自测完成

- UDP每seq保留有界原client归属，在pending/duplicate/late分类与过期变化前拒绝跨client错误回显；新增三态单测和真实proxy改写/投递负向，准确返回非0/valid=false且PID/fd回收。
- 修复后UDP默认/clients16共8run有效，短组连续2次、UDP专项及隔离通过；新18run/28PID原始统计资源/清理与60文件指纹核对一致。公开样本明确历史TCP与修复后UDP代次，首轮FAIL及旧证据保留。
- Builder002 Ready for Review，待Reviewer复审；生产/CMake/旧回归未改，按Reviewer001复用历史三构建和无关路径，不提前关闭F-001或宣称阶段完成。

### 2026-09-09 V0.4/S1首轮独立审查需返工

- Reviewer001：FAIL / Reworking。独立Debug25/25、Release26/26、Production16run、短组重复/既有故障/隔离/普通uid权限通过，36run原始数据与59个owned PID回收核对无不一致。
- 额外完整runner与真实产品试验发现F-001（P2）：UDP跨client篡改回显仍被记成功并返回valid=true，违反完整身份校验和损坏非0要求；必须在D1范围内返工并独立复审，尚未Completed。保留本轮失败证据，不以既有测试通过替代最终验收。

### 2026-09-09 V0.4/S1工具实现与自测完成

- 新增Python3标准库TCP/UDP paired benchmark、独立echo/资源账本、有限参数与UDP节拍、sampled RTT/schema证据、失败回收和公开方法/模板/样本；生产src/配置/C++零改动。
- 原23项CTest保留，追加3项短工具测试：Debug25/25、Release26/26（含原expiry一次）、Production两协议默认/clients16最终16run有效；短组重复2次、并行/空格路径及正式故障/普通uid权限通过。
- 最终36正负run符合预期，59个owned PID回收、fixture fd相等、原始统计与资源独立重算及60文件指纹核对。Builder001 Ready for Review，尚待Reviewer独立验收与Leader Closing；不声称S2优化、S3正式性能报告或V0.4完成。

### 2026-09-09 V0.4/S1批准登记

- PM批准V0.4-S1-D1决策包，设计与审查方案同步为Approved / Ready for Builder，新增Leader002批准登记。
- 授权S1工具/文档实现、独立验收与收尾，完成后本地提交；范围和验收要求不变，未提前宣告实现或验收完成。

### 2026-09-09 V0.3/S3合并核验与V0.4/S1准备

- 远端main及v0.3-s3注释tag peeled核验281db01，包含S3实现5221421；v0.1/S1至v0.3/S3共9个tag本地/远端对象一致且均可从main到达。
- 本地main快进，从其创建codex/v0.4-s1；按用户要求安全删除四个已合并旧开发分支，本地仅保留main和新分支，远端分支/tag不删除。
- 形成V0.4-S1-D1 Draft、审查方案及准备报告，定义TCP/UDP benchmark方法、工具边界与验证要求；同步当前状态入口。仅准备，未实现、未运行性能测试或批准后续开发；下方历史交接保留原时间事实。

本文件记录项目已经完成、发布或合并的重要变化。未来计划写入 `ROADMAP.md`，架构说明写入 `ARCHITECTURE.md`，实现细节写入 Builder 报告，审查结果写入 Reviewer 报告，技术债写入 `TECH-DEBT-TRACKER.md`。

## Unreleased

### 2026-09-09 V0.3/S3与V0.3完成收尾

- S3-D1保持Approved，Builder001、Reviewer001独立PASS与Leader003收尾齐备；S3及V0.3开发范围Completed，六条版本标准全部满足。生产代码、配置与格式文件零差异；V0.4未启动。
- 独立Debug22/22（93.26s）、Release23/23（152.60s）、Production三场景/38CLI、10次ICMP采样、工具五路目标负向、Release新增重复/并行/空格路径通过；29个产品PID回收且fixture fd相等，55文件指纹一致。
- TD-004本V0.3健康/指标/公开故障手册与版本矩阵义务完成，未来XDP/benchmark保持；无返工或带债。Builder额外Debug expiry为已披露命令偏差；ICMP历史根因未定位，不宣称修复。
- 交父按授权执行本地提交，不提前宣称合并、标签或发布；下方保留历史交接记录。

### 2026-09-09 V0.3/S3 独立验收通过

- Reviewer001 PASS：双backend TCP/UDP部分摘除、全不可选、分步恢复、旧绑定与独立指标账本及真实TCP拒绝全部通过，无新增债项。
- 独立Debug快速22/22（93.26s）、Release23/23（152.60s，唯一一次expiry），Production三场景/38项普通uid CLI/check静态通过；Release新增重复2次、并行2组、空格路径及五路工具负向通过。29个run（24正向、5故意负向）PID全部回收、fixture fd相等，55文件指纹一致，生产/C++零变化。
- 10次新namespace ICMP采样及完整回归未复现历史失败，根因仍未定位。当前Closing，待Leader同步S3/V0.3与TD-004后再由父按授权提交；未代签版本Completed。

### 2026-09-09 V0.3/S2合并与S3准备

- S2已由PR8合并，origin/main与远端v0.3-s2注释标签peeled核验为2f5c26d，含实现dfcffe6且树一致；S2独立复审PASS和历史返工结论保持。
- 新分支codex/v0.3-s3基于2f5c26d；形成V0.3-S3-D1 Draft故障矩阵、审查计划与准备决策，Awaiting PM Decision，尚未实施验证或获开发批准。
- 计划补双backend故障/恢复、连接拒绝与公开V0.3六条验收矩阵，保留旧ICMP失败证据边界；不新增产品功能、不提前宣告V0.3完成。历史交接记录保留。

### 2026-09-09 V0.3/S2 完成收尾

- 同V0.3-S2-D1保持Approved，Builder002、Reviewer002 PASS及Leader003收尾完成，S2 Completed；R-S2-01异常漏drop、R-S2-02正式fd断言缺失均关闭，无带债验收。S3未开始，V0.3整体尚未完成。
- 最终独立Debug19/19（48.74s）、Release20/20（110.44s，含原60s expiry）、Production两协议/38项普通uid CLI通过；静态check无网络/指标输出，五负向各exit1、23产品PID已回收、51文件指纹一致。
- TD-004 metrics规格义务完成，未来规格继续跟踪。旧ICMP首轮单次失败未在最终完整回归复现，原断言未改，不宣称根因已定位。父将按用户授权完成本地提交，下方保留各次交接历史。

### 2026-09-09 V0.3/S2 独立复审通过

- Reviewer002 PASS，R-S2-01/02关闭，无新增债项；原异常探针error1/drop1，五路正式收包断言及fd失败传播经独立正负验证。
- Reviewer独立新目录Debug19/19（48.74s）、Release20/20（110.44s，含原60s expiry），Production两协议指标/38项普通uid CLI/check零网络与零指标输出通过；五类Release负向各退出1，51指纹匹配。首轮旧ICMP单次失败根因未声称解决，本轮完整回归未复现。
- 当前Closing，交Leader同步阶段与TD-004后再由父按授权提交；保留001 FAIL和全部历史。

### 2026-09-09 V0.3/S2 Builder返工001

- 修复R-S2-01：已取得有效UDP包后create/选择异常先计一次drop，再原样传播；Error仍由control计，不重复正常空选择或成功发送计数。
- 修复R-S2-02：正式UDP事件测试补结束fd比较及失败传播；未发现产品fd泄漏，纠正Builder001对此证据覆盖的过度声明。新增真实收包后异常/正常失败/拒绝/成功控制以及两个返工负向。
- 独立rework-01重新构建验证：Debug19/19（48.92s）、Release20/20（109.58s，含原60s expiry）、Production两协议指标/普通uid38CLI/check纯静态均通过；原三类与新增两类Release负向均退出1。Reviewer旧ICMP首次失败原因仍未归因，本次完整Debug未复现且原断言未改。
- 原批准基线保持，不覆盖Builder001/Reviewer001；Builder002 Ready for Review，待Reviewer复审与Leader收尾。

### 2026-09-09 V0.3/S2 首轮独立审查

- Reviewer结论FAIL / Reworking：R-S2-01为UDP已取得数据报后建flow服务异常漏drop，R-S2-02为正式UDP指标事件测试缺fd结束比较；独立补验未发现fd泄漏，均待正式返工。
- 独立Release20/20（109.64s）、Production38项普通uid CLI/两协议指标/纯静态check通过，三类Release负向各退出1。Debug首轮18/19，旧UDP ICMP项单独重跑通过，保留原失败，修复后需完整复验。尚未阶段完成或提交。

### 2026-09-09 V0.3/S2 实现与 Builder 自测

- 新增独立可选metrics=off|stderr，默认off无collector/周期；schema=1快照记录实际业务生命周期、即时成功提交字节/UDP包、拒绝/drop/error/timeout及只读健康状态。TCP connecting计入会话，UDP零长成功也计一包。
- 错误在诊断限频前计，关闭不重复原错误；ready/每秒periodic/清理后final或error。同步stderr可能阻塞业务、SIGPIPE或留半行；已返回失败禁用后续指标，无新线程/端口/生产依赖。
- 新增5项测试保留原15项：Builder Debug19/19（48.81s）、Release20/20（109.72s，含原60s expiry）；Production两协议指标、普通uid CLI38项、check strace零网络调用/零指标行通过。三类Release实际落点错误副本均被正式测试检出，退出1。
- 新增metrics完整规格并同步配置/网络语义/README/架构/运行手册；沿用.h/.cpp同目录和函数定义间空行。当前Ready for Review，尚未独立验收/LeaderClosing；不宣告S3完成。

### 2026-09-09 V0.3/S1合并与S2准备

- S1已由PR7合并，origin/main与远端v0.3-s1注释标签peeled核验为9971818b，含实现0780b7d。按用户要求补齐19个cpp函数定义间空行及格式规则，不改变功能。
- 新分支codex/v0.3-s2基于9971818b；形成V0.3-S2-D1 Draft设计、审查计划和自包含准备决策，Awaiting PM Decision。仅准备，尚未实现指标或获得开发批准。
- README/ROADMAP/TD-004入口同步；metrics规格等待S2实现与独立验收，不提前关闭义务。历史交接记录保留。

### 2026-09-09 V0.3/S1 完成收尾

- V0.3-S1-D1保持Approved，Builder001、Reviewer001独立PASS及Leader003收尾齐备，S1 Completed；S2/S3未开始，V0.3整体尚未完成。
- 独立Debug14/14、Release15/15（含原60s expiry）、Production两协议健康和29项普通uid CLI通过；三类负向均检出，42文件指纹核对一致、16个健康测试产品PID已回收。
- ROADMAP与TD-004同步：健康规格义务完成，指标/XDP/benchmark后续义务保留。无新增技术债或未解决发现；成果尚未提交或发布。下方保留各次交接历史。

### 2026-09-09 V0.3/S1 实现与独立验收

- 实现可选 `health_check=off|tcp_connect`，默认 off 保持 TCP/UDP 兼容；独立非阻塞 checker、Unknown/Healthy/Unhealthy、连续2成功/3失败、固定完成后1s/超时1s，资源失败区分 local_error。
- control 对新会话/flow 过滤健康资格，空集合不推进轮询/不 fallback，旧 TCP 连接和 UDP flow 保持绑定。UDP 显式启用依赖同 IP/端口有代表性的 TCP 健康端点；check-config 保持纯静态。
- 新增4项测试保留原11项：Builder Debug快速14/14（34.84s）、Release完整15/15（94.62s，含60s expiry），Production构建/29项普通uid CLI/两协议健康冒烟通过；strace网络调用0，三类Release错误副本均被正式测试检出（退出1）。
- 健康规格、配置/调度/TCP/UDP语义、README/架构及运行手册同步。Python3仅新增产品测试fixture依赖；生产无第三方依赖。不含S2指标或S3完整故障矩阵；Reviewer独立验收PASS，当前Closing，待Leader收尾。

- Reviewer独立Debug14/14（35.37s）、Release15/15（95.09s），Production无测试构建/29项普通uid CLI/两协议健康通过；静态check网络调用0，三类独立Release负向各退出1，16个健康测试产品PID回收。修正README已知限制旧“无健康检查”措辞，无新增技术债。

### 2026-09-09 V0.3/S1准备

- 已核验V0.2/S3合并及标签7821b25（含1b16d31），从origin/main创建并切换codex/v0.3-s1；33文件验收指纹一致。历史未提交/发布描述保留当时事实。
- 形成V0.3-S1-D1 Draft设计、审查计划和准备决策包，Awaiting PM Decision；未实现探活，未重跑长测试。

### 2026-09-09 V0.2/S3与V0.2完成收尾

- Reviewer001 PASS后，Leader003核对S3七条AC与V0.2六条标准、33文件指纹及独立验证证据，S3和V0.2开发范围Completed。
- TD-004本版本配置/scheduler/完整UDP语义及系统矩阵义务完成，后续版本文档继续跟踪；无新债或未解决发现。生产src与原示例零改动，未提交/推送/合并/发布S3，不启动V0.3。下方记录保留各次交接的历史状态。

### 2026-09-09 V0.2/S3 独立审查

- Reviewer 独立 Debug 快速10/10（16.67秒）、Release完整11/11（76.88秒）；唯一P3静默60500ms、同一源端点与目标A→B，生产源码/参数未改。Production快速组及24项普通用户CLI通过。
- 两模式快速各3次、Release双组并行与空格路径、四类工具负向及三类旧负向均符合预期；公开手册完整流程退出0、六端口重绑，117条产品子PID及7个手动PID全部回收。1024仅静态默认值和内部缩容动态语义，未测真实满载。
- 独立结论PASS，无新增阻塞或技术债；见 `docs/reviewer/reports/V0.2/S3-report-001.md`。当前Closing，交Leader最终同步S3/V0.2；未提交或发布。

### 2026-09-09 V0.2/S3 实现与 Builder 自测

- 在原 UDP 产品测试上新增 wildcard/流隔离/伪造控制、停止A后的新flow恢复和原生产60秒过期模式；生产src及原示例零改动。
- 新增公开Python标准库UDP工具、完整单shell手动流程、运行手册和V0.2六条完成矩阵；容量1024明确为静态默认值确认而非满载实测。
- 当前注册11项；Builder Debug快速10/10、Release全套11/11（含一次P3实测静默60500ms）通过，Production快速组/CLI、两模式快速各3次、Release并行两组和空格路径通过；四类新增及三类继承负向均准确失败并清理。完整手动步骤含六端口重绑通过。
- 当前 Ready for Review，独立Reviewer与Leader收尾待进行，未提交或发布。报告 `docs/builder/reports/V0.2/S3-report-001.md`；完整UDP语义未改变。

### 2026-09-09 V0.2/S3 准备

- 已核验 S2 合并提交 c6927c0 与远端 main、v0.2-s2 标签 peeled commit 一致，包含 S2 实现03094cc；下方未提交/发布说明保留当时事实。
- 切换 codex/v0.2-s3 并形成 V0.2-S3-D1 Draft 设计、审查计划、准备报告；Awaiting PM Decision，未编码。31个S2源码/测试指纹一致，未重复已验收全套测试。

### 2026-09-08 V0.2/S2 完成收尾

- Reviewer001 PASS 后，Leader 核验 D1 批准、31 文件指纹、独立 9/9 及清理证据，完成阶段/路线图/债项同步；S2 Completed，D1 保持 Approved。见 `docs/leader/reports/V0.2/S2-report-003.md`。
- TD-004 的最小 UDP flow 规格义务完成；无返工、新增技术债或未解决发现。S3 完整语义/产品矩阵未开始，V0.2 整体未完成；本阶段未提交发布。以下交接记录保留当时状态。

### 2026-09-08 V0.2/S2 独立审查

- Reviewer 独立最终源码 Debug/Release 全套各 9/9（14.07/14.54 秒），普通用户纯测试各 3/3；Production Release 无测试构建、24 项 CLI 和真实 UDP 冒烟通过。网络验证使用临时 user/net namespace 的独立 lo，未改变宿主网络。
- 真实 ICMP、wildcard 源地址、零长/65507/截断及实际 fd 复用验证通过；错误回包、零长误作 EOF、错误选择三类实际产品 Release 临时突变均在目标断言退出 1，54 条产品子 PID 全部回收。
- 独立结论 PASS，无新增阻塞或技术债；见 `docs/reviewer/reports/V0.2/S2-report-001.md`。当前 Closing，交 Leader 最终同步；S3 尚未开始，未提交或发布。

### 2026-09-08 V0.2/S2 实现与 Builder 自测

- 新增独立 UDP reactor、IP_PKTINFO 实际目标 flow key 和回复源 IP、每 flow 已连接后端 socket；复用统一 Scheduler，无新配置键或 TCP reactor 变化。
- 明确零长/65507/截断、立即发送整包丢弃、60 秒空闲、1024 容量、错误隔离及 token 生命周期，新增最小 UDP flow 规格和必要测试。
- Builder Debug/Release 全套各 9/9（最终 14.10/14.39 秒）；Production Release 构建、CLI 及真实 UDP 冒烟通过；三个 Release 负向模型准确失败并清理。大包测试在临时网络命名空间独立 lo 完成，保留外部 loopback0 路由丢弃大包的环境证据，未改宿主网络。
- 当前 Ready for Review，待独立 Reviewer 和 Leader 收尾；S3 尚未开展，未提交或发布。证据见 `docs/builder/reports/V0.2/S2-report-001.md`。

### 2026-09-08 V0.2/S2 准备与 S1 标签核验

- S1 已合并至 53236b1，远端 main 与注释标签 v0.2-s1 的 peeled commit 一致；用户已完成标签推送，未据此声称创建 GitHub Release。下方阶段未提交/发布措辞保留其当时事实。
- 基于该提交切换 codex/v0.2-s2，形成 V0.2-S2-D1 Draft 设计、审查计划和准备报告，Awaiting PM Decision；UDP 实现未开始，无提交推送。已核对 16 个 S1 源码指纹一致，无需重复历史回归。

### 2026-09-08 V0.2/S1 完成收尾

- Reviewer002 PASS、F-001 Closed 后，Leader 核验源码指纹与验收证据，完成阶段、路线图及技术债状态同步；V0.2/S1 Completed，基线 D1 保持 Approved。见 `docs/leader/reports/V0.2/S1-report-003.md`。
- TD-004 的 scheduler 和配置扩展义务完成，UDP 等后续规格继续按原阶段推进；无新增债项或未解决发现。S2/S3 未开始；本阶段未提交、推送或发布。下方记录保留各次交接当时状态。

### 2026-09-08 V0.2/S1 文档修正与独立复审

- 通用 TCP 运行手册当前全套预期已更新为 7/7，说明新增 v02_scheduler_unit，并保留 V0.1/S3 历史记录；F-001 关闭。
- Reviewer002 复核源码指纹与七项测试注册一致，复用首轮独立产品证据，结论 PASS，无新增技术债。见 `docs/reviewer/reports/V0.2/S1-report-002.md`；当前 Closing，交 Leader 最终同步。

### 2026-09-08 V0.2/S1 独立首轮审查

- Reviewer 独立 Debug/Release 全套各 7/7（4.83/4.64 秒），Production Release 无测试构建与 25 项 CLI 检查通过；Release 错误 XOR 及健康后端代替失败后端的临时副本均准确退出 1。
- 首轮结论 FAIL，仅 F-001：通用 TCP 运行手册全套预期仍为 6/6，需 Builder 最小文档修正；产品及测试无需返工。见 `docs/reviewer/reports/V0.2/S1-report-001.md`。当前 Reworking，尚未完成 Leader 收尾。

### 2026-09-08 V0.2/S1 实现与 Builder 自测

- 新增可选 `protocol`/`scheduler` 严格配置及兼容默认值、统一 Scheduler/工厂和独立 round-robin；TCP 已接入，失败仍消耗一次选择且不重试。
- UDP 可静态校验，运行在任何网络资源创建前明确拒绝；同步配置、调度规格和帮助说明，未实现 UDP 数据面。
- Builder 独立 Debug/Release 全套各 7/7（最终 4.66/4.49 秒）；Release 无测试构建与完整 CLI 验证通过。实际字节验证默认/显式 A/B/A/B、失败/B/失败/B及占用端口 UDP 拒绝，Release 故意失败检查均退出 1。
- 当前 Ready for Review，尚待独立 Reviewer 验收与 Leader 收尾；报告 `docs/builder/reports/V0.2/S1-report-001.md`。未提交或发布。

### 2026-09-08 V0.2/S1 准备记录

- 从本地 main 的 S3 合并提交 `544c8d8` 创建并切换到 `v0.2-s1`，形成设计草案、审查计划及准备报告，当前为 Draft，未实现新功能。
- 新目录 Debug 构建通过，现有 CTest 6/6 通过（4.66 秒）；首次沙箱 socket 权限失败另存证据，可用环境复测通过。
- 本地提交图已证明 S3 合并；下方 S3 收尾时的未合并说明属于历史状态。远端查询因本机代理不可连接失败，本轮未核验远端发布状态。

### 2026-09-08 S3 实现与独立审查

- 新增真实产品 TCP 验收入口、动态端口演示后端、独立证据目录和有界清理，覆盖二进制轮询、1 MiB + 17 字节半关闭、后端停机恢复及启动失败；没有改变 S1/S2 产品契约。
- 新增可跟踪本地运行手册与 V0.1 验收矩阵；README 手动和自动路径均实跑通过。
- Reviewer 独立 Debug/Release 全套各6/6，s3各连续3次及双实例并行通过；生产无测试构建通过。错误数据/fixture提前退出/缺ready三类共6次准确退出1并回收，另缺换行ready、错误路径与用法检查通过。
- 独立审查 PASS，无新增阻塞或技术债；见 `docs/reviewer/reports/V0.1/S3-report-001.md`。Leader 已核对全部七条版本完成标准并完成收尾：S3 与 V0.1 开发范围 Completed，见 `docs/leader/reports/V0.1/S3-report-003.md`。尚未发布，S3 未提交、推送、合并或打标签。


### 2026-09-08 S3 开发准备

- 核验远端v0.1-s2标签并从S2合并后的main创建codex/v0.1-s3。
- 补齐S3-D1设计、审查计划及准备报告，聚焦真实产品进程验证与V0.1验收，准备当时状态为 Draft（后续批准和完成见上条）。
- 独立目录Debug构建和既有回归5/5通过；这是准备当时的前置记录，彼时 S3 尚未实现。


### 2026-09-08 S2-R001 测试返工与独立复审

- 修复反向半关闭后端断言未传播的假阳性：父测试通过独立管道在2秒内确认后端完整字节校验成功；失败消息、异常退出及未完成均失败。
- Debug/Release全套各5/5、s2标签各3/3；临时强制退出4、错误预期和后端异常三个负向副本均按预期退出1并明确诊断。只修复测试及交接文档，产品契约未变；详见 `docs/builder/reports/V0.1/S2-report-002.md`。Reviewer独立复审PASS：Debug/Release全套各5/5、标签各3/3；退出/错误数据/异常/缺消息四类负向副本共八次均准确退出1，S2-R001关闭。见 `docs/reviewer/reports/V0.1/S2-report-002.md`，Leader已收尾，S2 Completed；见 `docs/leader/reports/V0.1/S2-report-004.md`。

### 2026-09-08 S2 实现与首轮审查（历史，返工已关闭）

- 新增 `--run`、固定顺序轮询和单线程 LT epoll 双向 TCP 转发；非阻塞 connect 查询 SO_ERROR，单会话错误不终止服务。
- 新增每方向64KiB队列、背压恢复、排空后半关闭、5秒连接/60秒空闲截止、1024会话上限、token隔离及信号退出；停止不保证在途字节排空。
- 新增真实TCP集成、定向HUP/旧token状态验证、TCP语义规格及前台echo演示fixture。Builder Debug/Release各5/5、s2 label复跑3/3和README冒烟通过；逐项证据见 `docs/builder/reports/V0.1/S2-report-001.md`。Reviewer独立Debug/Release各5/5、s2 3/3及补充产品探针通过，但故意失败的reverse-fin后端断言仍导致总测试PASS；首轮审查FAIL，当时 S2-R001 要求 Builder 修复断言传播后复审；现已由上方复审记录关闭。详见 `docs/reviewer/reports/V0.1/S2-report-001.md`；不代表S2验收完成或V0.1整体完成。

### 2026-09-07 S2 设计准备

- 新增 S2-D1 详细设计、审查计划和准备报告，明确 TCP 转发、背压、半关闭与核心验证边界；当时状态为 Draft，待开发批准；2026-09-08 已批准并完成，保留准备历史。
- 复查 S1 CTest 2/2通过、本机epoll及TCP半关闭可用；当时尚未实现或验收S2。
- 更新 README 与阶段状态入口，详见 `docs/leader/reports/V0.1/S2-report-001.md`。

### 2026-09-07 S1 实现与独立验收

- 新增无第三方依赖的 C++20 CMake/Ninja 工程和 `l4lb` CLI；只检查配置，尚不转发流量。
- 新增严格 IPv4 TCP 配置解析、首错行诊断、有界只读加载和示例配置；拒绝 FIFO/设备/目录，允许普通文件符号链接。
- 新增配置规格、真实 README 命令及 Debug/Release CTest 基线：各 2 个测试通过，含 357 项单元检查和 22 个 CLI 用例。
- 实际验证普通用户权限错误、配置 SHA256 不变和 Release 故意失败返回 1；原始证据及限制见 `docs/builder/reports/V0.1/S1-report-001.md`。独立验收记录见下一条。

- Reviewer 独立干净 Debug/Release 构建均通过：各 CTest 2/2、357 项单元检查、22 个 CLI 用例；另有 56 项进程探针通过，首错、编码、大小及非阻塞文件路径符合 S1 契约。验收 PASS，Leader 已完成阶段收尾（Completed，见 `docs/leader/reports/V0.1/S1-report-003.md`）；仅 S1 完成，V0.1 尚未整体完成，详见 `docs/reviewer/reports/V0.1/S1-report-001.md`。

### 2026-09-07 启动准备

- 补齐 V0.1/S1 详细设计与审查计划，明确配置、CLI、测试基线及验收标准。
- 修正角色指南路径引用为实际的 `GUIDE.md`，补充 README 当前阶段入口。
- 增加 CMake 构建目录和测试临时文件忽略规则，保留原有本地协作文档忽略规则。
- 验证临时 C++20 工程配置、编译、CTest、epoll 和本机 TCP 双向回环通信；项目实现及项目测试尚未开始。
- 详细结果见 `docs/leader/reports/V0.1/S1-report-001.md`。

以下通用条目保留初始化时的历史记录；当前实现状态以上述 S2 完成及 S1 条目为准。

### 新增

- 新增项目入口文档 `README.md`，说明项目定位、当前状态、环境要求、快速开始占位、项目结构、测试验证策略、文档索引、开发流程和已知限制。
- 新增长期架构文档 `ARCHITECTURE.md`，明确 C++20 用户态 L4 负载均衡器、控制面、用户态数据面、XDP/eBPF 数据面和基础设施层的职责边界。
- 新增总体路线图 `ROADMAP.md`，定义从 `V0.1` 用户态 TCP 转发骨架到 `V1.2` XDP L4 fast path 原型的版本范围、阶段划分、禁止范围和完成标准。
- 新增技术债跟踪文档 `TECH-DEBT-TRACKER.md`，记录初始化阶段需要跨阶段跟踪的构建命令、配置选择、XDP 环境和长期规格文档风险。
- 新增仓库级 Agent 工作规则 `AGENTS.md`，说明文档权威来源、全局工作原则、文档语言要求和变更记录要求。
- 新增 Leader、Builder 和 Reviewer 角色规则，分别位于 `docs/leader/GUIDE.md`、`docs/builder/GUIDE.md` 和 `docs/reviewer/GUIDE.md`。
- 为三个角色补充长期规格文档职责：Leader 负责规划，Builder 负责随实现维护，Reviewer 负责审查实现、测试、报告和文档是否一致。

### 变更

- 将项目方向收敛为 C++20 用户态 TCP/UDP L4 负载均衡器优先，XDP/eBPF fast path 后续演进。
- 明确项目与高性能 HTTP 服务器的分层区别：HTTP 服务器聚焦应用层协议处理，本项目聚焦传输层 TCP/UDP 四层转发、后端调度、UDP flow table、健康检查和 XDP/eBPF fast path。
- 明确本仓库不引入 DPDK 数据面；DPDK L3 forwarding 或 NAT 可作为后续独立项目方向。
- 明确 WSL2 适合用户态开发和基础验证，XDP/eBPF 阶段优先使用云服务器 Linux 环境进行功能验证和收尾。
- 明确云服务器性能结果只代表对应云环境，不泛化为物理网卡 native XDP 极限性能。
- 明确当前 README 中的 CMake/Ninja/CTest 命令属于目标工程形态，需等待 `V0.1 / S1` 工程骨架建立后变为真实可运行命令。

### 修复

- 修正根 `AGENTS.md` 中角色文档路径示例，使其指向实际路径 `docs/leader/GUIDE.md`、`docs/builder/GUIDE.md` 和 `docs/reviewer/GUIDE.md`。
- 清理 `README.md` 初始化过程中的重复内容，保留单一中文版项目入口文档。

### 安全

- 当前未引入代码、网络服务、文件删除、权限修改、外部命令执行或敏感信息处理。
- 架构文档已记录后续涉及 XDP attach、网络接口修改和系统权限时必须集中封装并明确错误提示。

### 验证

- 使用 `Get-Content -Encoding UTF8 README.md` 读回确认 README 内容为单一中文版。
- 使用 `Get-Content -Encoding UTF8 ARCHITECTURE.md` 读回确认架构文档已写入并保持 UTF-8 可读。
- 使用 `Get-Content -Encoding UTF8 ROADMAP.md` 读回确认路线图已写入版本范围、阶段划分和长期演进方向。
- 使用 `Get-Content -Encoding UTF8 TECH-DEBT-TRACKER.md` 读回确认技术债条目、风险观察和下一阶段检查点已写入。
- 使用 `Get-Content -Encoding UTF8 AGENTS.md` 及三个角色 `GUIDE.md` 读回确认职责补充已写入。

### 相关文档

- 项目入口：`README.md`
- 架构文档：`ARCHITECTURE.md`
- 路线图：`ROADMAP.md`
- 技术债：`TECH-DEBT-TRACKER.md`
- 仓库规则：`AGENTS.md`
- Leader 规则：`docs/leader/GUIDE.md`
- Builder 规则：`docs/builder/GUIDE.md`
- Reviewer 规则：`docs/reviewer/GUIDE.md`

### V0.3/S3 Builder交付（2026-09-09，待独立验收）

- 新增TCP/UDP双backend部分/全故障与分步恢复、真实TCP拒绝无重试的产品验证及独立nonce/指标账本。
- 工具错误传播、PID/fd清理、重复/并行/空格路径已验证；提供普通clone运行手册与六条版本矩阵。
- Builder Debug完整23/23、Release23/23，Production三场景及普通uid38CLI通过。10次独立ICMP采样未复现历史偶发，根因未定位；生产功能无变化，Reviewer/Leader尚待完成。
