param(
    [Parameter(Mandatory)][string]$SuiteRoot,
    [string]$PythonExe = '',
    [string]$Xsa = '',
    [switch]$AllowRtosTimer,
    [switch]$AllowCandidateDma,
    [switch]$AllowCapture,
    [switch]$Network,
    [string]$HardwareReceipt = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $projectRoot 'build\embedded_platform'
if($AllowCandidateDma){$outputRoot=Join-Path $projectRoot 'build\dma_platform'}
if($AllowCapture){$outputRoot=Join-Path $projectRoot 'build\capture_bsp'}
if($Network){if(!$AllowCapture){throw 'Network requires AllowCapture'};$outputRoot=Join-Path $projectRoot 'build\capture_network_bsp'}
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$launcherRunId = [Guid]::NewGuid().ToString('N')
$statusPath = Join-Path $outputRoot 'status.json'
$initial = @{status='RUNNING'; stage='launcher'; launcher_run_id=$launcherRunId; board_execution_authorized=$false; init_execution_authorized=$false}
[IO.File]::WriteAllText($statusPath, ($initial | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $outputRoot 'status.txt'), "RUNNING embedded_platform BUILD_ONLY`n", [Text.UTF8Encoding]::new($false))
$envNames = @('PATH','PYTHONPATH','PYTHONHOME','PYTHONDONTWRITEBYTECODE','PYTHONUTF8','XILINX_VITIS','XILINX_VIVADO','VITIS_EMBEDDED_INSTALL','JAVA_OPTS','LOPPER_DTC','LOPPER_CPP')
$savedEnv = @{}
foreach ($name in $envNames) { $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$exitCode = 1
try {
    # Vendor library discovery reads UTF-8 BOM YAML without encoding on Windows.
    $env:PYTHONUTF8 = '1'
    if (([int]$AllowRtosTimer.IsPresent + [int]$AllowCandidateDma.IsPresent + [int]$AllowCapture.IsPresent) -gt 1) { throw 'Hardware profile opt-ins are mutually exclusive' }
    $SuiteRoot = [IO.Path]::GetFullPath($SuiteRoot)
    $vitisRoot = Join-Path $SuiteRoot 'Vitis'
    if (-not $PythonExe) { $PythonExe = Join-Path $SuiteRoot 'Vivado\tps\win64\python-3.13.0\python.exe' }
    if ($AllowCapture -and -not $Xsa) { $Xsa = Join-Path $projectRoot 'build\capture_hardware\capture_dma.xsa' }
    if (-not $Xsa) { $Xsa = Join-Path $projectRoot 'build\ps_hardware\ps_only.xsa' }
    $PythonExe = [IO.Path]::GetFullPath($PythonExe)
    $Xsa = [IO.Path]::GetFullPath($Xsa)
    if ($AllowCapture) {
        if (-not $HardwareReceipt) { $HardwareReceipt = Join-Path (Split-Path -Parent $Xsa) 'status.json' }
        $contractArgs = @('-B', (Join-Path $PSScriptRoot 'capture_bsp_contract.py'), '--xsa', $Xsa, '--hardware-receipt', $HardwareReceipt, '--output', (Join-Path $outputRoot 'input_contract.json'))
        & $PythonExe @contractArgs *> (Join-Path $outputRoot ('input_' + $launcherRunId + '.log'))
        if ($LASTEXITCODE -ne 0) { throw 'Capture hardware input contract failed; inspect input_contract.json' }
    }
    $dtc = Join-Path $vitisRoot 'bin\dtc.exe'
    $gnuBin = Join-Path $SuiteRoot 'gnu\aarch32\nt\gcc-arm-none-eabi\bin'
    $cpp = Join-Path $gnuBin 'arm-none-eabi-cpp.exe'
    if ($AllowCapture) {
        # Some suite layouts keep dtc with Vivado. Embedded and ARM GNU gates
        # below remain mandatory even when this alternate bundled dtc exists.
        if (-not (Test-Path -LiteralPath $dtc -PathType Leaf)) { $dtc = Join-Path $SuiteRoot 'Vivado\bin\dtc.exe' }
        $required = @((Join-Path $vitisRoot '.vitis_embedded'),
            (Join-Path $vitisRoot '.settings64-Vitis_Embedded_Development.bat'),
            (Join-Path $gnuBin 'arm-none-eabi-gcc.exe'),
            (Join-Path $gnuBin 'arm-none-eabi-ar.exe'),
            (Join-Path $gnuBin 'arm-none-eabi-readelf.exe'), $cpp, $dtc)
        $missing = @($required | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
        if ($missing.Count -gt 0) { throw ('Missing Embedded build components: ' + ($missing -join ', ')) }
    }
    foreach ($path in @($PythonExe, $Xsa, $dtc, $cpp, (Join-Path $vitisRoot '.vitis_embedded'), (Join-Path $vitisRoot 'cli\vitis\__init__.py'))) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing build input/tool: $path" }
    }
    $javaUserDir = Join-Path $outputRoot ('java_home_' + $launcherRunId)
    [IO.Directory]::CreateDirectory($javaUserDir) | Out-Null
    $env:LOPPER_DTC = $dtc
    $env:LOPPER_CPP = $cpp
    $env:JAVA_OPTS = (($savedEnv['JAVA_OPTS'] + ' -Duser.home="' + $javaUserDir + '"').Trim())
    $env:XILINX_VITIS = $vitisRoot
    $env:XILINX_VIVADO = Join-Path $SuiteRoot 'Vivado'
    $env:VITIS_EMBEDDED_INSTALL = 'true'
    $env:PYTHONHOME = $null
    $env:PYTHONDONTWRITEBYTECODE = '1'
    $env:PYTHONPATH = (@('cli','cli\python-packages\win64','cli\python-packages\site-packages','cli\proto','scripts\python_pkg') | ForEach-Object { Join-Path $vitisRoot $_ }) -join ';'
    $env:PATH = (@((Split-Path -Parent $PythonExe), (Join-Path $vitisRoot 'bin'), (Join-Path $vitisRoot 'lib\win64.o'), (Join-Path $SuiteRoot 'Vivado\bin'), $gnuBin, (Join-Path $SuiteRoot 'Vivado\tps\win64\cmake-4.0.1\bin'), (Join-Path $vitisRoot 'tps\win64\cmake-4.0.1\bin'), $savedEnv['PATH'])) -join ';'
    $runtimeLog = Join-Path $outputRoot ('runtime_' + $launcherRunId + '.log')
    try {
        $ErrorActionPreference = 'Continue'
        & $PythonExe -B (Join-Path $PSScriptRoot 'build_embedded_platform.py') --runtime-preflight --vitis-root $vitisRoot *> $runtimeLog
    } finally { $ErrorActionPreference = 'Stop' }
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $runtimeLog -Pattern '^PASS embedded_runtime BUILD_ONLY$' -Quiet)) {
        throw ('Embedded Python runtime preflight failed: ' + (Get-Content -LiteralPath $runtimeLog -Raw).Trim())
    }
    $fixtureLog = Join-Path $outputRoot ('fixtures_' + $launcherRunId + '.log')
    try {
        $ErrorActionPreference = 'Continue' # Windows PowerShell treats unittest stderr as NativeCommandError.
        & $PythonExe -B (Join-Path $projectRoot 'tests\python\test_embedded_platform.py') *> $fixtureLog
    } finally { $ErrorActionPreference = 'Stop' }
    if ($LASTEXITCODE -ne 0) { throw 'Embedded platform fixture tests failed; see ignored log' }
    $buildLog = Join-Path $outputRoot ('build_' + $launcherRunId + '.log')
    $buildArgs = @('-B', (Join-Path $PSScriptRoot 'build_embedded_platform.py'), '--xsa', $Xsa, '--output-root', $outputRoot, '--launcher-run-id', $launcherRunId)
    if ($AllowCapture) { $buildArgs += @('--allow-capture', '--hardware-receipt', $HardwareReceipt) }
    if ($Network) { $buildArgs += '--network' }
    if ($AllowRtosTimer) { $buildArgs += '--allow-rtos-timer' }
    if ($AllowCandidateDma) { $buildArgs += '--allow-candidate-dma' }
    try {
        $ErrorActionPreference = 'Continue'
        & $PythonExe @buildArgs *> $buildLog
    } finally { $ErrorActionPreference = 'Stop' }
    $nativeExit = $LASTEXITCODE
    $record = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    if ($nativeExit -ne 0 -or $record.status -cne 'PASS' -or $record.launcher_run_id -cne $launcherRunId -or $record.board_execution_authorized -ne $false -or $record.init_execution_authorized -ne $false -or $record.client_disposed -ne $true -or $record.client_mode -cne 'VITIS') {
        throw 'Embedded platform build/receipt failed; inspect ignored status.json and build log'
    }
    Write-Output 'PASS embedded_platform BUILD_ONLY'
    $exitCode = 0
} catch {
    $failed = @{status='FAIL'; stage='launcher'; launcher_run_id=$launcherRunId; error=$_.Exception.Message; board_execution_authorized=$false; init_execution_authorized=$false}
    # Preserve detailed fresh Python failures, while invalidating any stale PASS.
    if (Test-Path -LiteralPath $statusPath) {
        try {
            $existing = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
            if ($existing.launcher_run_id -ceq $launcherRunId -and $existing.status -ceq 'FAIL') { $failed = $existing }
        } catch { }
    }
    [IO.File]::WriteAllText($statusPath, ($failed | ConvertTo-Json -Depth 30), [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $outputRoot 'status.txt'), "FAIL embedded_platform BUILD_ONLY`n", [Text.UTF8Encoding]::new($false))
    Write-Output 'FAIL embedded_platform BUILD_ONLY'
    Write-Output $_.Exception.Message
} finally {
    foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process') }
}
exit $exitCode
