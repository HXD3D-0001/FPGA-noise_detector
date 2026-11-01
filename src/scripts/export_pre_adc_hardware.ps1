param([Parameter(Mandatory)][string]$VivadoRoot,[Parameter(Mandatory)][string]$PythonExe,[switch]$Ethernet)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'build/pre_adc_hardware'
$profile='CARRIER_SD0'
$tclArgs=@();$checkArgs=@()
if($Ethernet){
 $build=Join-Path $root 'build/pre_adc_network_hardware'
 $profile='CARRIER_SD0_ETHERNET'
 $tclArgs=@('-tclargs','--ethernet');$checkArgs=@('--ethernet')
}
New-Item -ItemType Directory -Force $build|Out-Null
$receipt=@{status='RUNNING';scope="PRE_ADC_PROFILE_${profile}_XSA_ONLY";board_execution_authorized=$false;init_execution_authorized=$false}
$state=Join-Path $build 'status.json'
$receipt|ConvertTo-Json|Set-Content -LiteralPath $state -Encoding utf8
$saved=$env:XILINX_VIVADO
Push-Location $root
try {
 $files=@(Get-ChildItem rtl -Filter *.v)+@(Get-ChildItem generated -Filter *.mem)+@(Get-Item bd/create_pre_adc_bd.tcl,bd/ax7021_pre_adc_profile.tcl,scripts/export_pre_adc_hardware.ps1,scripts/check_pre_adc_hardware.py,scripts/check_capture_hardware.py,scripts/check_dma_hardware.py,tests/python/test_ps_hardware.py,tests/python/test_pre_adc_network_hardware.py,build/fft_ip/lfn_fft/lfn_fft.xci)
 $hashes=@{};foreach($f in $files){$hashes[$f.FullName]=(Get-FileHash -LiteralPath $f.FullName).Hash}
 $receipt.source_hashes=$hashes
 $env:XILINX_VIVADO=$VivadoRoot
 foreach($f in @('pre_adc_dma.xsa','ps_configuration.tsv','validation.json')){if(Test-Path -LiteralPath "$build/$f"){Remove-Item -LiteralPath "$build/$f"}}
 & (Join-Path $VivadoRoot 'bin/vivado.bat') -mode batch -nojournal -log "$build/launcher.log" -source bd/create_pre_adc_bd.tcl @tclArgs *> "$build/console.log"
 if($LASTEXITCODE -ne 0 -or !(Select-String -LiteralPath "$build/console.log" -Pattern "^PASS pre_adc_hardware_export PROFILE_${profile}_BUILD_ONLY$" -Quiet) -or (Select-String -LiteralPath "$build/console.log" -Pattern '^ERROR:|^FAIL ' -Quiet)){throw 'Pre ADC profile export failed; inspect console.log'}
 foreach($p in $hashes.Keys){if((Get-FileHash -LiteralPath $p).Hash -ne $hashes[$p]){throw "Input changed: $p"}}
 & $PythonExe -B scripts/check_pre_adc_hardware.py "$build/pre_adc_dma.xsa" --output "$build/validation.json" @checkArgs *> "$build/check.log"
 if($LASTEXITCODE -ne 0 -or !(Select-String -LiteralPath "$build/check.log" -Pattern "^PASS pre_adc_hardware PROFILE_${profile}_METADATA_ONLY$" -Quiet)){throw 'Pre ADC variant validation failed; see check.log'}
 # Exact finalized-profile set asserted from the exported artifact itself.
 # Candidate-chain check_capture_hardware.py is NOT reused here: its external
 # peripheral fail-closed baseline (all EN_*=0) is candidate-only by design.
 $ps=Get-Content -LiteralPath "$build/ps_configuration.tsv"
 $expected=[ordered]@{'CONFIG.PCW_EN_UART1'='1';'CONFIG.PCW_UART1_UART1_IO'='MIO 12 .. 13';'CONFIG.PCW_UART1_BAUD_RATE'='115200';
  'CONFIG.PCW_EN_UART0'='0';'CONFIG.PCW_EN_SDIO0'='1';'CONFIG.PCW_SD0_SD0_IO'='MIO 40 .. 45';'CONFIG.PCW_SD0_GRP_CD_ENABLE'='1';
  'CONFIG.PCW_SD0_GRP_CD_IO'='MIO 10';'CONFIG.PCW_EN_SDIO1'='0';'CONFIG.PCW_EN_QSPI'='0';'CONFIG.PCW_EN_USB0'='0';'CONFIG.PCW_EN_ENET0'='0';
  'CONFIG.PCW_PRESET_BANK0_VOLTAGE'='LVCMOS 3.3V';'CONFIG.PCW_PRESET_BANK1_VOLTAGE'='LVCMOS 1.8V';
  'CONFIG.PCW_UIPARAM_DDR_PARTNO'='MT41J256M16 RE-125';'CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH'='32 Bit'}
 if($Ethernet){$expected['CONFIG.PCW_EN_ENET0']='1'}
 foreach($k in $expected.Keys){
  $line=$ps|Where-Object {$_ -like "$k`t*"}|Select-Object -First 1
  if(-not $line -or ($line -split "`t",2)[1] -ne $expected[$k]){throw "Profile set mismatch: $k expected '$($expected[$k])' got '$line'"}}
 $receipt.xsa_sha256=(Get-FileHash -LiteralPath "$build/pre_adc_dma.xsa").Hash
 $receipt.validation_sha256=(Get-FileHash -LiteralPath "$build/validation.json").Hash
 $receipt.status='PASS'
 Write-Output "PASS pre_adc_hardware PRE_ADC_PROFILE_${profile}_XSA_ONLY"
} catch {if(Test-Path -LiteralPath "$build/pre_adc_dma.xsa"){Remove-Item -LiteralPath "$build/pre_adc_dma.xsa"};$receipt.status='FAIL';$receipt.error=$_.Exception.Message;throw}
finally {$env:XILINX_VIVADO=$saved;$receipt|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $state -Encoding utf8;Pop-Location}
