# AX7021 监测上位机

源代码对应独立窗口 1.5.1。展示时将电脑网口与开发板 ETH0 直连，使用同一子网的地址；上位机也支持 USB 串口 115200 8N1。固件、接线和启动见工程包根 README.md 与 board/。

在此目录执行 python server.py，打开程序输出的本地 URL。串口模式需要 Python 3.12 或更新版本及 pyserial 3.5。桌面窗口入口是 window/main.cjs。独立程序构建方法见 build/README.md。
