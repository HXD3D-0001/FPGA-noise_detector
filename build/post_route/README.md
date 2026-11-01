# 布局布线前后对比

两次均使用 Vivado 2026.1、同一 capture_dma_wrapper 顶层、xc7z020clg484-2 器件与 50 MHz 主逻辑约束。基线时间为 2026-10-26 07:26，修正后为同日 19:49。报告为各自布局布线完成后的结果。

修复内容是 FFT 两个缓冲区均为空时清空操作产生的错误 GAP。相同输入下，旧 RTL 给出首包标志 0x0D 并触发断言，修正为 0x09 后通过原空缓冲用例。修正保留了真实队列丢弃时的缺口标志。整机 LUT 增加 15 个，寄存器减少 108 个，BRAM Tile 和 DSP 用量分别保持 47 和 32 个。WNS 由 +0.477 ns 变为 +0.282 ns；两版 TNS 均为 0 ns。

`before_utilization.rpt` 与 `before_timing_summary.rpt` 保留基线工具原始报告正文，去除了主机名、运行命令行。两份基线 RPT 及最终报告均见相邻 `../result/`。`comparison.json` 记录两次实现的资源与时序结果。

