# 住宅低频噪声监测

AX7021（Zynq-7000，xc7z020clg484-2）采集麦克风信号，在板上分析 31.5～250 Hz 的十个频带。上位机展示频谱与趋势，支持住户标记、整场记录和导出。

## 提交目录

| 目录 | 内容与入口 |
|---|---|
| `src/` | [源码说明](src/guide.pdf)，含 RTL、PS 固件和上位机。[上位机使用说明](src/host_monitor/guide.pdf) |
| `sim/` | [验证说明](sim/guide.pdf)、测试入口及 RTL 结果 |
| `build/` | [构建步骤](build/guide.pdf)、[构建脚本](build/rebuild.ps1)、[两版实现结果](build/post_route/results.pdf) |
| `board/` | [接线、启动与上板复现](board/guide.pdf) |
| `data/` | [实测说明](data/guide.pdf)、十分钟长测、独立窗口短测与整场导出短测数据 |
| `skill/` | [技能说明](skill/README.md)，含 XSim 仿真验收与 PYNQ Overlay 接口检查 |
| `report/` | [设计报告](report/design_report.pdf)、[大模型协作记录](report/ai_collaboration.pdf) |

## 快速验证

在工程根目录使用 Python 3.12 或更新版本执行：

```shell
python -B sim/verify.py
python -B data/check.py
python -B skill/selftest.py
python -B skill/pynq_overlay/selftest.py
```

以上命令在电脑上运行。完整构建使用 Windows x64、PowerShell 7 和 Vivado/Vitis 2026.1，具体前置条件见构建步骤。实板试验与离线回放的适用范围见实测说明。

## 许可

项目自有源码及技能使用 [MIT 许可](license.txt)；第三方材料见[第三方说明](third_party.pdf)。
