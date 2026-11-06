# Bless 中文导读

本文吸收早期中文《架构概览》《可控熵注入设计概述》《项目目录结构》和相关指南的讲解，按当前实现重新组织。
它与英文主题文档共同维护，参数、接口和验证状态通过链接引用当前参考文档。
旧译稿中的固定阈值、历史代码行号、完成百分比和未附测量条件的性能结论不作为当前依据。

## Bless 用来回答什么问题

Bless 是基于 DPDK 的可控流量发生器。
除发包速率外，它还观察报文字段的分布、字段之间的依赖以及发送时序，帮助解释一种负载究竟在测试什么。
更高的熵不必然代表更真实或更好的流量：固定服务端口、租户与 VTEP 的绑定、协议相关的包长都可能是实验需要保留的结构。

组织一个实验时，分别确定三个问题：

| 维度 | 决定什么 | 观察什么 |
|---|---|---|
| 报文形态 | 协议权重、地址/端口池、包长、封装与变异 | Shannon、Min-Entropy、MI，以及抓包结果 |
| 注入行为 | 批量、间隔、抖动、速率、流交错和握手状态 | TX 统计、PSD、提交延迟、连接事件 |
| 测量与反馈 | 采样、统计窗口、配置基线、是否调整 PPS | 样本证据、目标差距和调参前后的可比结果 |

这三类控制不能互相替代。
扩大 IP 池不等于扩大已建立连接表；增加时序抖动不保证降低源/目的 IP 的互信息；提高速率也不保证短窗口覆盖更多有效组合。

## 系统数据流

```text
YAML + CLI -> 配置树与协议注册 -> 每个 worker 的启动配置
                                  |
                                  v
                           构造 / 封装 / 变异
                                  |
                                  v
                           限速与提交 -> DPDK PMD
                                  |
                           每 worker 采样环
                                  |
                                  v
                       主核计算熵、MI、PSD 与统计
                                  |
                    HTTP / Prometheus / WebSocket
```

运行时控制另有一条受限路径：字段描述表先验证类型、范围和可修改性，再通过互斥锁及原子字段发布变化。
它不是整棵配置树的热替换。
发送成功计数通常表示 PMD 接收了报文，不等于对端接收；端到端结论需要 RX、抓包或 DUT 侧证据。

## 从哪里开始

| 目的 | 阅读顺序 |
|---|---|
| 理解架构取舍 | [设计理由与边界](design/decisions.md)，然后阅读[架构总览](../overview.md) |
| 配置与运行 | [中文运行指南](guides/runtime.md)，再查[配置参考](../guides/config.md) |
| 解释熵和互信息 | [场景讲解](guides/scenarios.md)、[诊断基线](../concepts/entropy-interpretation.md)和[仪表盘字段](../guides/dashboard.md) |
| 验证连接与时延 | [双端握手实验](../guides/handshake-experiment.md) |
| 配置虚拟化开发环境 | [QEMU 指南](../guides/qemu-development.md) |
| 开发协议或配置模块 | [协议扩展接口](../overview.md#42-protocol-extension-api)和[配置模块边界](../reference/config-architecture.md) |
| 分析性能 | [profiling](../guides/profiling.md)、[基准方法](../reference/benchmarks.md)和[设备分层比较](../reference/three-layer-comparison.md) |
| 查看本次项目审查 | [2026-09-27 审查报告](../reference/project-assessment-2026-09-27.md)，含复现证据、验证边界和修复优先级 |
| 评估完成程度 | [测试覆盖边界](../reference/testing-coverage.md)和[整改路线图](../reference/remediation-roadmap.md) |
| 维护文档与技能 | [项目开发、文档维护、流程复用与技能更新指南](../contributing/project-development-documentation-and-skills.md) |

## 目录与证据

`src/` 保存实现，`include/` 保存头文件，`conf/` 保存实验配置，`test/` 和 `fuzz/` 保存不同层次的验证入口，`tools/` 保存仪表盘与测量工具。
配置已拆分为文件、YAML 树、值、数组和协议域模块；熵统计计算位于 `src/entropy_stats.c`，不能继续照搬旧文中指向 `worker.c` 的行号。

设计提案、已有代码、自动化覆盖和真实硬件测量是不同证据。
早期《业内展示路线图》的价值在于提示复现、部署、观测和验证都需要交付；其完成百分比不能替代当前测试或发布记录。

完整主题索引见[文档总览](../overview.md#further-reading)。
