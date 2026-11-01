param(
    [Parameter(Mandatory)][string]$SuiteRoot,
    [Parameter(Mandatory)][string]$PythonExe
)
$ErrorActionPreference = 'Stop'
$src = Join-Path (Split-Path $PSScriptRoot -Parent) 'src'
$suite = (Resolve-Path -LiteralPath $SuiteRoot).Path
$pythonExe = (Resolve-Path -LiteralPath $PythonExe).Path
$vivadoRoot = Join-Path $suite 'Vivado'
$vivado = Join-Path $vivadoRoot 'bin/vivado.bat'
$savedVivado = $env:XILINX_VIVADO
function RunNative([string]$command, [string[]]$arguments) {
    & $command @arguments
    if ($LASTEXITCODE -ne 0) { throw "Failed: $command $arguments" }
}
Push-Location $src
try {
    $env:XILINX_VIVADO = $vivadoRoot
    RunNative $vivado @('-mode','batch','-source','scripts/generate_fft_ip.tcl','-nojournal')
    & ./scripts/export_pre_adc_hardware.ps1 -VivadoRoot $vivadoRoot -PythonExe $pythonExe
    RunNative $vivado @('-mode','batch','-source','scripts/implement_pre_adc_hardware.tcl','-nojournal')
    & ./scripts/export_pre_adc_hardware.ps1 -Ethernet -VivadoRoot $vivadoRoot -PythonExe $pythonExe
    $hw = Join-Path $src 'build/pre_adc_network_hardware'
    & ./scripts/build_embedded_platform.ps1 -SuiteRoot $suite -AllowCapture -Network -Xsa "$hw/pre_adc_dma.xsa" -HardwareReceipt "$hw/status.json"
    if ($LASTEXITCODE -ne 0) { throw "BSP build failed. See $src/build/capture_network_bsp/status.json" }
    RunNative $pythonExe @('-B','scripts/build_capture_image.py','--suite-root',$suite,'--network')
    RunNative $pythonExe @('-B','scripts/build_product_boot.py','--suite-root',$suite)
    Write-Output "BOOT.BIN: $(Join-Path $src 'build/product_boot/BOOT.BIN')"
} finally {
    $env:XILINX_VIVADO = $savedVivado
    Pop-Location
}
