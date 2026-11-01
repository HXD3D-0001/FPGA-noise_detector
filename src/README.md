# 源码索引

rtl、bd、constraints 和 generated 分别保存 Verilog 设计、Zynq 系统连接、管脚与时序约束及滤波系数。ps 是处理系统应用，shared 保存数据协议。host_monitor 是 Windows 上位机。scripts 和 tools 是构建及模型验证的被调用模块，tests 保留相应测试平台和输入。对外入口在工程包顶层的 build/ 和 sim/。FPGA FFT、DMA、BSP 和 FSBL 来自用户本机 AMD 套件；运行时、许可证和商业 IP 实现未随源码分发。
