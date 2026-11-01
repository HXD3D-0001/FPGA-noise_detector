param([Parameter(Mandatory=$true)][string]$VivadoRoot)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$build=Join-Path $root 'build/sim_fft_scheduler'
New-Item -ItemType Directory -Force $build | Out-Null
Push-Location $build
try {
 & (Join-Path $VivadoRoot 'bin/xvlog.bat') -sv (Join-Path $root 'rtl/fft_scheduler.v') (Join-Path $root 'tests/rtl/tb_fft_scheduler.sv') 2>&1 | Tee-Object 'compile.log'
 if($LASTEXITCODE -ne 0){throw 'scheduler compilation failed'}
 & (Join-Path $VivadoRoot 'bin/xelab.bat') tb_fft_scheduler -s tb_fft_scheduler 2>&1 | Tee-Object 'elaborate.log'
 if($LASTEXITCODE -ne 0){throw 'scheduler elaboration failed'}
 & (Join-Path $VivadoRoot 'bin/xsim.bat') tb_fft_scheduler -runall 2>&1 | Tee-Object 'run.log'
 if($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath 'run.log' -Pattern 'Fatal:|Error:|FAIL' -Quiet)){throw 'scheduler simulation failed'}
 if(!(Select-String -LiteralPath 'run.log' -Pattern '^PASS fft_scheduler' -Quiet)){throw 'scheduler PASS marker missing'}
 Write-Output 'PASS fft_scheduler'
} finally {Pop-Location}
