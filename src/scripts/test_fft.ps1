param([Parameter(Mandatory=$true)][string]$VivadoRoot,
      [Parameter(Mandatory=$true)][string]$PythonExe,
      [Parameter(Mandatory=$true)][string]$CCompiler,
      [string]$LicenseFile)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'build/sim_fft_pipeline'
New-Item -ItemType Directory -Force $build | Out-Null
$status=Join-Path $build 'status.json'
function Status([string]$value){
 @{schema_version=1;stage='fft';status=$value;utc=[DateTime]::UtcNow.ToString('o')} |
  ConvertTo-Json | Set-Content -LiteralPath $status -Encoding utf8
}
function Checked([string]$exe,[string[]]$arguments,[string]$log,[string]$marker=''){
 & $exe @arguments *> $log
 $code=$LASTEXITCODE;$content=Get-Content -LiteralPath $log -Raw
 if($code -ne 0 -or $content -match '(?im)^ERROR:|^Fatal:|^Error:|\$fatal' -or
    ($marker -and $content -notmatch "(?m)^$marker\s*$")){throw "FFT stage failed; see $log"}
}
Status 'RUNNING'
$savedVivado=$env:XILINX_VIVADO;$savedLicense=$env:XILINXD_LICENSE_FILE
Push-Location $root
try{
 foreach($name in @('fft_pipeline_result.json','fft_packets.txt','fft_timing.txt','fft_window.txt','simulate.log')){
  $stale=Join-Path $build $name
  if(Test-Path -LiteralPath $stale){Remove-Item -LiteralPath $stale}
 }
 foreach($tool in @($PythonExe,$CCompiler,(Join-Path $VivadoRoot 'bin/vivado.bat'))){
  if(!(Test-Path -LiteralPath $tool -PathType Leaf)){throw 'Missing explicit FFT verification tool'}
 }
 $env:XILINX_VIVADO=$VivadoRoot
 if($LicenseFile){$env:XILINXD_LICENSE_FILE=(Resolve-Path -LiteralPath $LicenseFile).Path}
 Checked $PythonExe @('-X','utf8','-m','unittest','discover','-s','tests/python','-p','test_fft_evidence.py','-v') (Join-Path $build 'evidence_tests.log') 'OK'
 & (Join-Path $PSScriptRoot 'test_fft_scheduler.ps1') -VivadoRoot $VivadoRoot *> (Join-Path $build 'scheduler.log')
 if((Get-Content (Join-Path $build 'scheduler.log') -Raw) -notmatch '(?m)^PASS fft_scheduler\s*$'){throw 'Scheduler PASS missing'}
 & (Join-Path $PSScriptRoot 'test_fft_result_store.ps1') -VivadoRoot $VivadoRoot *> (Join-Path $build 'store.log')
 if((Get-Content (Join-Path $build 'store.log') -Raw) -notmatch '(?m)^PASS fft_result_store\s*$'){throw 'Store PASS missing'}
 Checked (Join-Path $VivadoRoot 'bin/vivado.bat') @('-mode','batch','-source','scripts/generate_fft_ip.tcl','-nojournal','-log',"$build/generation.log") (Join-Path $build 'generation.console.log') 'PASS fft_ip_generation'
 Checked $PythonExe @('-X','utf8','tests/python/check_fft_pipeline.py','generate','--build',$build,'--xci','build/fft_ip/lfn_fft/lfn_fft.xci') (Join-Path $build 'vectors.log') 'PASS fft_pipeline_vectors'
 Checked $CCompiler @('-std=c11','-Wall','-Wextra','-Werror','-O2','-static','-Ishared','shared/protocol.c','tests/host/fft_packet_gold.c','-o',"$build/fft_packet_gold.exe") (Join-Path $build 'codec.compile.log')
 Checked (Join-Path $VivadoRoot 'bin/vivado.bat') @('-mode','batch','-source','scripts/simulate_fft_pipeline.tcl','-nojournal','-log',"$build/launcher.log") (Join-Path $build 'launcher.console.log') 'PASS fft_pipeline_simulation_launcher'
 $simulation=Get-Content (Join-Path $build 'simulate.log') -Raw
 if($simulation -match '(?im)Fatal:|Error:|\$fatal' -or $simulation -notmatch '(?m)^PASS fft_axis_pipeline\s*$'){throw 'Actual FFT RTL assertion/PASS failure'}
 Checked $PythonExe @('-X','utf8','tests/python/check_fft_pipeline.py','check','--build',$build,'--encoder',"$build/fft_packet_gold.exe") (Join-Path $build 'comparison.log') 'PASS fft_pipeline_wire_and_model'
 Status 'PASS';Write-Output 'PASS fft'
}catch{Status 'FAIL';throw}finally{
 Pop-Location;$env:XILINX_VIVADO=$savedVivado;$env:XILINXD_LICENSE_FILE=$savedLicense
}
