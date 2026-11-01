param([Parameter(Mandatory=$true)][string]$VivadoRoot,
      [string]$SourceFile='', [string]$BuildName='sim_fft_result_store', [switch]$Synthesize)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if($BuildName -notmatch '^sim_fft_result_store[a-zA-Z0-9_]*$'){throw 'Invalid isolated build name'}
$negative=[bool]$SourceFile
if(!$SourceFile){$SourceFile=Join-Path $root 'rtl/fft_result_store.v'}
if($negative -and $Synthesize){throw 'Synthesis requires production source'}
$build=Join-Path $root "build/$BuildName"
New-Item -ItemType Directory -Force -Path $build | Out-Null
Push-Location $build
try {
 & (Join-Path $VivadoRoot 'bin/xvlog.bat') -sv $SourceFile (Join-Path $root 'tests/rtl/tb_fft_result_store.sv') *> compile.log
 if($LASTEXITCODE -ne 0){throw 'Store compile failed'}
 & (Join-Path $VivadoRoot 'bin/xelab.bat') tb_fft_result_store -s sim_fft_result_store *> elaborate.log
 if($LASTEXITCODE -ne 0){throw 'Store elaborate failed'}
 & (Join-Path $VivadoRoot 'bin/xsim.bat') sim_fft_result_store -runall *> simulate.log
 $code=$LASTEXITCODE;$log=Get-Content simulate.log -Raw
 if($code -ne 0 -or $log -match '(?im)Fatal:|Error:|\$fatal' -or $log -notmatch '(?m)^PASS fft_result_store\s*$'){throw 'Store behavior check failed'}
 Write-Output 'PASS fft_result_store'
 if($Synthesize){
  $previous=$env:XILINX_VIVADO
  try {
   $env:XILINX_VIVADO=$VivadoRoot
   & (Join-Path $VivadoRoot 'bin/vivado.bat') -mode batch -source (Join-Path $root 'scripts/synthesize_fft_result_store.tcl') -log synth.log -journal synth.jou *> synth.console.log
   $code=$LASTEXITCODE;$log=Get-Content synth.console.log -Raw
   if($code -ne 0 -or $log -notmatch '(?m)^PASS fft_result_store_synthesis\s*$'){throw 'Store synthesis failed'}
   Write-Output 'PASS fft_result_store_synthesis'
  } finally {$env:XILINX_VIVADO=$previous}
 }
} finally {Pop-Location}
