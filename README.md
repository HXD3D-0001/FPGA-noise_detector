# FPGA-noise_detector

住宅低频噪音监测与特征分析。AX7021 通过 MAX4466 和 PCM1808 采集 48 kHz 音频，在 Zynq-7000 上分析中心频率 31.5～250 Hz 的十个频带，将频谱、事件与住户标记写入 SD 卡。竞赛展示由电脑网口直连 ETH0。提交的 Windows 上位机源码对应 1.5.1。

## 开发板源码

开发板使用 ALINX AX7021（`xc7z020clg484-2`）。板上的 FPGA 完成音频采集与定点处理，ARM 处理器负责分析、存储和通信。下面的目录是构建板端固件的源码入口。

| 部分 | 源码 | 用途 |
| --- | --- | --- |
| FPGA 可编程逻辑 | [Verilog RTL](src/rtl/) · [滤波与窗系数](src/generated/) | I²S 接收、跨时钟域、三级 FIR 抽取、能量计算、FFT 调度和 AXI 数据分包。 |
| Vivado 系统与板级约束 | [Block Design Tcl](src/bd/) · [XDC 与时序约束](src/constraints/) | 在本机生成 Zynq PS、FFT 和 DMA 所需的工程，设置 AX7021 的引脚和时钟。 |
| ARM 端固件 | [PS 源码](src/ps/) · [共享协议](src/shared/) | DMA 接收、十频带分析、事件与标记、SD 写入与回读、以太网和维护串口。 |
| 镜像与上板 | [构建入口](build/rebuild.ps1) · [接线与启动](board/README.md) | 生成 PL 位流、硬件描述、BSP 和应用 ELF，最后打包 SD 启动文件。 |

## 运行环境

| 要做的事 | 本机准备 |
| --- | --- |
| 从源码构建开发板 | Windows x64、PowerShell 7、Python 3.12+、Vivado/Vitis 2026.1。安装 Zynq-7000 器件支持、可用的 FFT IP 许可、Vitis Embedded 与 ARM 工具链。工程使用套件提供的 FreeRTOS、FatFs、lwIP 和 FSBL。 |
| 运行上位机，通过网线连板 | Windows x64、Python 3.12+ 和浏览器。该方式使用 Python 标准库。电脑有线网口接板上 ETH0。 |
| 运行上位机，通过 USB 串口连板 | 在上述环境中安装 `pyserial==3.5`，并确认 Windows 已识别板卡对应的 COM 口。串口参数为 115200、8N1。 |
| 自行打包 1.5.1 Windows 独立窗口 | 在本机准备 Python 3.13.16 嵌入式运行包、Electron 45.2.1 Windows x64 和 pyserial 3.5 wheel。打包脚本校验下载文件的 SHA-256；版本、来源和命令见[构建说明](build/README.md)。 |

### 构建开发板

在仓库根目录打开 PowerShell 7。将套件根目录换成本机实际位置，该目录下应同时包含 `Vivado`、`Vitis` 和 `gnu`。

```powershell
$suite = Read-Host '输入 AMD 2026.1 套件根目录'
./build/rebuild.ps1 -SuiteRoot $suite -PythonExe (Get-Command python).Source
```

启动镜像输出到 `src/build/product_boot/BOOT.BIN`。接线、SD 启动设置和板上核对步骤见[板卡说明](board/README.md)。

### 运行上位机

在仓库根目录运行以下命令。若板卡使用 DHCP，将示例地址替换成板卡实际地址。

```powershell
python src/host_monitor/server.py --open --board http://169.254.70.21
```

如使用 USB 串口，先执行 `python -m pip install pyserial==3.5`，再运行 `python src/host_monitor/server.py --open`，在界面选择“USB 串口”并连接。源码运行不需要下载 Electron；只有制作独立窗口发行包时才需准备它。

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

