# FPGA-noise_detector

住宅低频噪音监测与特征分析。AX7021 通过 MAX4466 和 PCM1808 采集 48 kHz 音频，在 Zynq-7000 上分析中心频率 31.5～250 Hz 的十个频带，将频谱、事件与住户标记写入 SD 卡。竞赛展示由电脑网口直连 ETH0。提交的 Windows 上位机源码对应 1.5.1。

## 竞赛资料导航

| 目录 | 内容及入口 |
| --- | --- |
| [src](src/README.md) | RTL、约束、PS 固件、1.5.1 上位机及生成脚本。 |
| [sim](sim/README.md) | 离线验证、RTL 仿真和错误首帧标志的回归结果。 |
| [build](build/README.md) | Vivado/Vitis 2026.1 构建入口、布局布线后报告及修正前后数据。 |
| [board](board/README.md) | 硬件连接、启动文件校验、板卡运行和只读状态查询。 |
| [data](data/README.md) | 十分钟观测与导出核对、逐频音源试验、显示版本验证。 |
| [skill](skill/README.md) | 可独立使用的 XSim 日志校验器、通过与故障示例及自检。 |
| [report](report/design_report.pdf) | 按自主选题要求独立编写的设计报告；[模型交互与纠错记录](report/ai_collaboration.md)。 |

## 快速验证

在项目根目录使用 Python 3.12 或更新版本执行：

```sh
python -B sim/verify.py
python -B data/check.py
python -B skill/selftest.py
```

安装 AMD 工具后，可依 [构建说明](build/README.md) 从源码生成位流与启动镜像。实板复现见 [板卡说明](board/README.md)。完整构建需要本地合法安装的 AMD 套件、器件支持和相关运行库。上位机源码支持以太网和 USB 串口；[运行与打包](src/host_monitor/README.md)。

## 如何解读提交数据

1.5.0 的十分钟试验采用原生串口与真实 ADC，SD 有效覆盖 606.208 / 610.234 s，六条标记同步。1.5.1 的屏幕与板端字段一致性在另一场短测中验证。数据目录保留频带数值、相对时间与统计结果，未纳入住户录音、自由文本备注或开发机身份信息。

## 开源范围

自有源代码和技能包按 [MIT](license.txt) 发布。第三方声明见 [third_party.md](third_party.md)。公开仓库从源码重建，不分发第三方工具链、运行时安装包及预编译的板端启动镜像和位流。

