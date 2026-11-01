param([Parameter(Mandatory=$true)][string]$VivadoRoot,
 [string]$BuildName='sim_shared_store',[switch]$Synthesize,[string]$RtlSource='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if($BuildName -notmatch '^sim_shared_store[a-zA-Z0-9_]*$'){throw 'Invalid isolated build name'}
$build=Join-Path $root "build/$BuildName"
New-Item -ItemType Directory -Force $build | Out-Null
$statusFile=Join-Path $build 'status.json'
@{status='RUNNING';evidence='RTL_ONLY'}|ConvertTo-Json|Set-Content $statusFile -Encoding utf8
if(!$RtlSource){$RtlSource=Join-Path $root 'rtl/spectrum_shared_store.v'}
Push-Location $build
$sharedHadEnv=Test-Path -LiteralPath Env:XILINX_VIVADO
$sharedSavedEnv=$env:XILINX_VIVADO
try {
 $env:XILINX_VIVADO=$VivadoRoot
 foreach($artifact in @('shared_store.dcp','utilization.rpt','simulate.log')){if(Test-Path -LiteralPath $artifact){Remove-Item -LiteralPath $artifact -Force}}
 New-Item -ItemType Directory -Force source_snapshot | Out-Null
 Copy-Item -LiteralPath $RtlSource -Destination source_snapshot/spectrum_shared_store.v -Force
 Copy-Item -LiteralPath (Join-Path $root 'tests/rtl/tb_spectrum_shared_store.sv') -Destination source_snapshot/tb_spectrum_shared_store.sv -Force
 $rtlSnapshot=Join-Path $build 'source_snapshot/spectrum_shared_store.v'
 $benchSnapshot=Join-Path $build 'source_snapshot/tb_spectrum_shared_store.sv'
 & (Join-Path $VivadoRoot 'bin/xvlog.bat') -sv $rtlSnapshot $benchSnapshot *> compile.log
 if($LASTEXITCODE -ne 0){throw 'Shared bank compile failed'}
 & (Join-Path $VivadoRoot 'bin/xelab.bat') tb_spectrum_shared_store -s sim_shared_store *> elaborate.log
 if($LASTEXITCODE -ne 0){throw 'Shared bank elaboration failed'}
 & (Join-Path $VivadoRoot 'bin/xsim.bat') sim_shared_store -runall *> simulate.log
 $exit=$LASTEXITCODE;$log=Get-Content simulate.log -Raw
 if($exit -ne 0 -or $log -match '(?im)Fatal:|Error:|FAIL' -or $log -notmatch '(?m)^PASS spectrum_shared_store\s*$'){throw 'Shared bank behavior failed'}
 if($Synthesize){
  $env:LFN_SHARED_SOURCE=$rtlSnapshot
  @'
foreach dir [glob -nocomplain -types d [file join $::env(XILINX_VIVADO) data XilinxTclStore support *] [file join $::env(XILINX_VIVADO) data XilinxTclStore tclapp * *]] {lappend auto_path $dir}
create_project -in_memory -part xc7z020clg484-2
read_verilog $::env(LFN_SHARED_SOURCE)
synth_design -top spectrum_shared_store
report_utilization -file utilization.rpt
set ram0 [llength [get_cells -hier -filter {IS_PRIMITIVE && REF_NAME =~ RAMB* && NAME =~ *ram0*}]]
set ram1 [llength [get_cells -hier -filter {IS_PRIMITIVE && REF_NAME =~ RAMB* && NAME =~ *ram1*}]]
set ram36 [llength [get_cells -hier -filter {IS_PRIMITIVE && REF_NAME =~ RAMB36*}]]
set ram18 [llength [get_cells -hier -filter {IS_PRIMITIVE && REF_NAME =~ RAMB18*}]]
set ff [llength [get_cells -hier -filter {IS_PRIMITIVE && REF_NAME =~ FD*}]]
set lut [llength [get_cells -hier -filter {IS_PRIMITIVE && REF_NAME =~ LUT*}]]
puts "SHARED_STORE_RESOURCES RAM0=$ram0 RAM1=$ram1 RAMB36=$ram36 RAMB18=$ram18 FF=$ff LUT=$lut"
if {$ram0 < 6 || $ram1 < 6 || $ram0+$ram1 != $ram36+$ram18 || $ram36*2+$ram18 > 32 || $ram36*2+$ram18 < 24} {error "FAIL unexpected shared RAM geometry or extra stores"}
if {$ff > 4000 || $lut > 7000} {error "FAIL unexpected shared control footprint"}
write_checkpoint -force shared_store.dcp
puts "PASS spectrum_shared_store_synthesis"
exit 0
'@|Set-Content synthesize.tcl -Encoding utf8
  & (Join-Path $VivadoRoot 'bin/vivado.bat') -mode batch -source synthesize.tcl -nojournal -log synthesis.log *> synthesis.console.log
  $exit=$LASTEXITCODE;$log=Get-Content synthesis.console.log -Raw
  if($exit -ne 0 -or $log -notmatch '(?m)^PASS spectrum_shared_store_synthesis\s*$'){throw 'Shared bank RAM synthesis gate failed'}
  Select-String -LiteralPath synthesis.console.log -Pattern '^SHARED_STORE_RESOURCES ' | ForEach-Object {$_.Line}
 }
 @{status='PASS';evidence='RTL_ONLY';synthesis=[bool]$Synthesize;rtl_sha256=(Get-FileHash $rtlSnapshot).Hash;bench_sha256=(Get-FileHash $benchSnapshot).Hash}|ConvertTo-Json|Set-Content $statusFile -Encoding utf8
 Write-Output 'PASS shared_store'
}catch{
 @{status='FAIL';evidence='RTL_ONLY';error=$_.Exception.Message}|ConvertTo-Json|Set-Content $statusFile -Encoding utf8
 throw
}finally{
 if($sharedHadEnv){$env:XILINX_VIVADO=$sharedSavedEnv}else{Remove-Item -LiteralPath Env:XILINX_VIVADO -ErrorAction SilentlyContinue}
 Remove-Item -LiteralPath Env:LFN_SHARED_SOURCE -ErrorAction SilentlyContinue
 Pop-Location
}
