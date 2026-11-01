# 构建与实现资料

将工程包解压到较短的本地路径。提供 Windows x64、PowerShell 7、Python 3.12+、AMD Vivado/Vitis 2026.1、Zynq-7000 器件库、嵌入式工具和有效许可。套件须包含 Vitis Embedded、ARM 工具链、FreeRTOS、FatFs、lwIP 和官方 FSBL 源。不要将安装包、许可证、个人凭据或构建缓存放入发布目录。

在工程包根目录执行 PowerShell 命令：

```powershell
./build/rebuild.ps1 -SuiteRoot $env:XILINX_SUITE -PythonExe (Get-Command python).Source
```

XILINX_SUITE 指向包含 Vivado、Vitis 和 gnu 的 2026.1 套件根目录。脚本生成 FFT IP、PL 位流、带以太网的 XSA、BSP、配置头文件、应用 ELF、FSBL 和 BOOT.BIN；日志与中间产物在 src/build 下。板卡运行见 board/，脚本自身不写启动介质。实现后检查 src/build/pre_adc_implementation 下的 utilization.rpt、timing_summary.rpt 和 gates.json。result/ 包含已完成的整机实现资料。

上位机源码运行见 src/host_monitor/README.md。独立窗口打包使用 Python 3.13.16 的 Windows x64 embeddable ZIP、Electron v45.2.1 Windows x64 ZIP 和 pyserial 3.5 wheel。按 src/scripts/package_host_monitor.py 中的官方来源取得文件，再执行：

```powershell
python ./src/scripts/package_host_monitor.py --runtime $pythonZip --window-runtime $electronZip --serial-wheel $pyserialWheel
```

其中三个变量分别填入本机对应归档文件的路径。打包器检查指定的运行时输入，保留各运行时自带许可及 pyserial 原始许可。工具路径和板卡序列号无需写入代码。

result/ 中的 before_ 报告为 2026-10-26 空 FFT 缓存标志修正前的同顶层整机结果。没有 before_ 前缀的时序和利用率报告为同日修正后的布局布线结果。dsp_ooc_synthesis.rpt 是 2026-10-19 独立 capture_processing_top 的历史模块级综合结果，不能拿来作为整机优化前的基线。
