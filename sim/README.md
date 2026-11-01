# 仿真与验证

在工程包根目录执行 `python -B sim/verify.py`。该入口运行硬件描述与平台契约检查、模拟串口测试以及整场记录和导出回归。整场测试在临时目录检查记录、标记与观测点的配对、两份 CSV 和图表输出。无需连接开发板。若只需重跑上位机整场用例，执行 `python -B -m unittest discover -s src/host_monitor -p "test_session_*.py"`。

安装 Vivado 2026.1 后执行 `./sim/run_rtl.ps1 -VivadoRoot $env:XILINX_VIVADO`。该脚本执行真实 XSim，检查 FFT 共享缓存描述符，包括空缓存清空后的 GAP 标志。通过条件为进程退出正常、无失败断言并出现 `PASS spectrum_shared_store`。复现旧版故障需使用修正前的 RTL；result.json 记录在两版实测中核对的字段。

对 4096 点 FFT 做 AMD C 模型与 RTL 逐帧对比时，在工程包根目录执行 `./src/scripts/test_fft.ps1 -VivadoRoot $env:XILINX_VIVADO -PythonExe (Get-Command python).Source -CCompiler (Get-Command gcc).Source`。需 NumPy、兼容 GCC、已安装的 Vivado FFT IP 及其模型依赖。综合、实现和板端实测分别见 build/、board/ 与 data/。
