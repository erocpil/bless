# 配置、运行与排错

本文保留旧中文配置、运行时、Docker、错误处理和观测指南的操作脉络。
具体命令和字段定义链接到当前主题参考，以免复制一套会独立过时的参数表。

## 先选实验配置，再覆盖参数

从已有 `conf/` 配置选择对应设备和工作模式，确认 DPDK main/worker lcore、端口、队列和进程 file-prefix。
YAML 保存可复现配置，CLI 用于本次启动覆盖；它们与运行时 WS 修改的可用范围不同。
默认值还要区分程序零初始化、样例 YAML、CLI 显式参数和 worker 的初始化回退值。
例如 `conf/config.yaml` 设置 `batch: 32`、`sample-interval: 10`，不能把样例值描述为所有启动方式的默认值。

构建和依赖见[Building](../../reference/BUILDING.md)，常用 CLI、配置语法和优先级见[配置指南](../../guides/config.md)。
从仓库根目录运行构建，不直接把 `src/Makefile` 当独立入口。

## 哪些值可以实时修改

当前 `src/runtime_field.c` 允许实时修改 `pps_rate`、`sample_interval`、`entropy_target`、`entropy_dim`、`entropy_adapt_gain`。
`seed` 是特殊命令，下次启动发送时应用。
`batch`、流量模型、突发容量、字节速率、握手参数等在运行时会被拒绝；停止再启动发送也不等于重启进程并重建启动配置。

需要控制命令时显式开启 `system.server.control_enable`。
非 loopback 监听还需要远程控制开关和有效 API key；HTTP 观测接口不因此自动获得 WS 密钥鉴权。
当前密钥有效长度为 16–255 字节，空环境值与未设置并不相同。
具体配置及浏览器连接方式见[API](../../reference/api.md)与[安全指南](../../guides/security.md)。

WS 数值字段应发送 JSON 数字：

```json
{"cmd":"set","key":"pps_rate","value":100000}
```

界面的 K/M/G 输入便利格式不表示原始 WS 接口接受任意数值字符串，也不能突破字段范围。
查询或控制失败时先查看返回错误和字段可修改性，不要把旧界面中的字段列表当作全部可写参数。

## 正确理解 PPS、字节速率和时间模型

`pps_rate` 的 token bucket 按 TX worker 分别维护。
`bps_rate` 的历史名字虽然含 bps，实际单位是 Byte/s；100 Mbit/s 对应 12,500,000 Byte/s，多个 worker 各自拥有预算。
记账使用 DPDK `pkt_len`，不包括 FCS、前导码和帧间隙，因此该数值不是精确链路线速。

固定延迟、抖动、指数批间隔和 Pareto ON-OFF 描述发送批次行为。
批量发送下，指数分布的批间隔不能被称为每个报文独立的 Poisson 到达。
提交延迟指标描述软件何时调用 PMD，无法单独证明报文何时离开网卡。
抖动应限制在正的有效间隔以内；当前无符号计算不会自动把负延迟钳制为零。
完整边界见[运行时参数](../../guides/runtime.md)。

## 用逐层实验定位故障

1. 先确认配置、设备选择和启动日志；解析模块返回错误，由启动入口决定是否终止。
2. 确认对应 worker 实际启动，区分单 worker 不可用与整个进程启动失败。
3. 看 TX 接受数、RX 和连接状态；HTTP 页面可访问不代表数据链路工作。
4. 观察采样关闭、样本不足、覆盖计数和条件维度 inactive，避免把没有观测误解成流量质量差。
5. 对双端测试先验证直连，再插入 DUT；对性能比较固定环境和采样条件。

使用[双端握手步骤](../../guides/handshake-experiment.md)建立功能基线。
Docker 默认 loopback 绑定的接口不能仅靠 `-p` 从宿主机访问，按[Docker 指南](../../guides/docker.md)选择容器内检查或明确配置监听策略。
错误返回和资源所有权见[错误处理](../../guides/error-handling.md)，指标与 Grafana 见[观测指南](../../guides/observability.md)。

## 复现与验证的边界

保存配置、seed、代码版本、worker 拓扑及测量窗口。
固定 seed 不要求所有时间字段或所有快照逐字节相同。
软件 PMD 可以验证控制路径或构造，不能替代 NIC 和 DUT 测量。
CI 中存在某个测试也不等于它覆盖完整生产路径；[测试覆盖表](../../reference/testing-coverage.md)区分辅助函数、生产模块、独立服务 fixture 和真实硬件验证。
