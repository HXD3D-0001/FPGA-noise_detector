# 技能说明

本包按赛题 3.3.5.2 提供两项技能，可分别使用。

| 任务 | 从哪里开始 | 已验证范围 |
|---|---|---|
| 通用 PYNQ Overlay 接口检查 | [使用方法与迁移示例](pynq_overlay/guide.pdf) | 15 项离线用例及真实 AX7021 工程元数据的双 DMA 实例回放；PYNQ 实板下载与外设功能待目标板验证 |
| XSim 仿真验收 | 本页下方操作步骤 | 九项基础用例及两份真实 FFT 仿真日志回放 |

## PYNQ 接口检查

在本目录运行 `python -B pynq_overlay/selftest.py`。换题目时指定目标 IP 的类型、版本及寄存器偏移；换兼容板时使用该板配套的设计文件。多个同型实例需要显式选择。接口不匹配时停止。15 项自测和双 DMA 实例回放均在电脑上执行，PYNQ 实板下载与外设功能未测试。[使用步骤及失效条件](pynq_overlay/guide.pdf)

## XSim 仿真验收

换题目时指定新测试平台的完成标志，并用已知故障验证脚本能识别该平台的错误输出。XSim 日志脚本在普通电脑上运行，无需开发板。

适合已经有测试平台、准备把仿真接入自动构建的团队。输入为一次 XSim 运行的完整文本输出、该进程的退出码和测试平台约定的通过标志。输出为可供构建脚本使用的通过或失败状态。脚本只使用 Python 3.8 及以上版本的标准库。

## 提炼过程

在 AX7021 板级开发中，团队曾故意将 LED 输出固定为常量。测试平台检测到翻转断言失败，XSim 却以 0 退出。团队随后将显式通过标志纳入验收。修正后的入口拒绝了常量输出，正确版本在 296 ns 内完成 30 个时钟上升沿的功能检查。低频监测工程也遇到过 FFT 缓冲区清空后首包标志错误，仿真在 82090 ns 输出 Fatal 诊断。两种经历说明自动判断需要同时核对进程、失败诊断和测试完成标志。

## 如何使用

在本目录运行以下命令，自检不需要安装 Vivado。

```text
python selftest.py
python verify_xsim.py examples/pass.txt --exit-code 0 --pass-line TB_DONE_OK
python verify_xsim.py examples/fault.txt --exit-code 0 --pass-line TB_DONE_OK
```

第二条应返回 0，第三条应返回 1。示例为人工构造的最小输入。故障文件故意同时含有成功行与 Fatal 行，用于验证失败诊断具有优先权。

接入自己的测试平台时，先定义一个唯一的整行标志，例如 TB_AUDIO_FIFO_OK。在所有检查均完成之后，只打印一次该标志。每次启动仿真前创建新的输出文件，保存完整输出，在下一条命令运行前捕获真实退出码。以下示例适用于 PowerShell 7 与已编译的 XSim 快照；把 tb_snapshot 和标志替换为自己的名称。

```powershell
xsim tb_snapshot -runall *> sim_current.txt
$simExit = $LASTEXITCODE
python verify_xsim.py sim_current.txt --exit-code $simExit --pass-line TB_AUDIO_FIFO_OK
exit $LASTEXITCODE
```

## 判定与负对照

| 输入特征 | 预期结果 |
|---|---|
| 退出码为 0，无失败诊断，成功行出现一次 | 通过 |
| 退出码非零，日志含成功行 | 拒绝 |
| 退出码为 0，日志仍有 Fatal、Error 或 SIM_FAIL 行 | 拒绝 |
| 仿真提前结束，标志缺失；多次运行被拼接，标志重复 | 拒绝 |

每次更换测试平台，应至少注入一个会触发断言的已知错误，核对脚本返回值，再恢复原设计运行正例。示例自检覆盖上述规则和“成功词仅出现在回显命令中”的情况。

## 用共享 FFT 缓冲区案例复查

两份节选取自 2026-10-26 同一测试平台的 XSim 记录，仅保留运行命令、判定行和结束命令。修正前，空缓冲区清空后的第一个描述符含多余的 GAP 位。修正后，同一测试平台输出整行 `PASS spectrum_shared_store`。在本目录执行下面两条命令，每次执行后用 PowerShell 的 `$LASTEXITCODE` 查看结果。

```powershell
python verify_xsim.py examples/fft_before.txt --exit-code 0 --pass-line "PASS spectrum_shared_store"
python verify_xsim.py examples/fft_after.txt --exit-code 0 --pass-line "PASS spectrum_shared_store"
```

第一条应返回 1，并指明故障出现在第 3 行。第二条应返回 0。两次回放都传入进程退出码 0，脚本仍能从第一份日志中识别失败。原始运行的对照结果见配套工程包的 `sim/result.json`。运行 `python selftest.py` 会依次检查九项基础用例及这两份真实日志节选。

## 适用边界

脚本按 UTF-8 文本和行首诊断词解析，忽略首尾空白及可选的 XSim 转录前缀 `#`。采用其他语言、编码或自定义断言格式时，需要扩展匹配规则并增加对应负对照。如果子测试预期产生 Error，应为该子测试定义独立判据，不能直接套用本规则。包装器若传错退出码、复用旧文件，或测试平台过早打印成功行，验收结果便无法证明本次运行完整。

通过只表示提交给脚本的这次仿真满足约定。组合逻辑或时序覆盖取决于测试平台，布局布线时序、板上采样和声学响应应分别验证。

## 协作时可复用的提问

“这是同一测试平台的正例与注入故障后的负例。请列出退出码、首条失败诊断、整行完成标志及出现次数，给出判定依据。随后设计一个能推翻当前判据的最小反例。”

此模板用于引导检查。最终构建状态由脚本和团队复核决定。
