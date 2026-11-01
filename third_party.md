# 第三方组件与资料

项目的 MIT 授权适用于本项目贡献者有权许可的内容。原有版权声明随对应文件保留。

- 板级参数参考 ALINX AX7021 资料。未随公开源码分发厂商手册和原理图。
- AMD Vivado/Vitis、FFT、AXI DMA、XPM、BSP、FSBL 以及随套件提供的驱动由使用者从其获许可的本地工具生成或链接。厂商工具与商业 IP 不纳入本项目的 MIT 授权。
- 上位机打包可使用 Electron、Python 嵌入式运行时和 pyserial。打包脚本指明来源并校验固定哈希；须保留各组件自带许可。pyserial 版权声明随 [上位机源](src/host_monitor/licenses/pyserial-license.txt) 提供。
- 实现报告节选自 Vivado 生成的本项目输出。运行环境、主机标识和绝对路径已去除。竞赛本地包中的已验证位流用于板上复现，公开仓库仅提供其校验值、工程源及重建步骤。

