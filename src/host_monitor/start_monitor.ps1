param(
 [string]$Board,
 [int]$Port=8765,
 [string]$PythonPath,
 [switch]$NoBrowser
)
$ErrorActionPreference='Stop'
try {
 if(!$PythonPath){
  $candidates=@()
  foreach($name in @('python','python3','py')){
   $command=Get-Command $name -CommandType Application -ErrorAction SilentlyContinue
   if($command){$candidates+=$command.Source}
  }
  # Use the existing local runtime when Python is not on PATH; never install one.
  $candidates+=Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
  foreach($candidate in $candidates){
   if(Test-Path -LiteralPath $candidate){
    & $candidate -c 'import sys; sys.exit(0 if sys.version_info >= (3,12) else 1)' 2>$null
    if($LASTEXITCODE -eq 0){$PythonPath=$candidate;break}
   }
  }
 }
 if(!$PythonPath){throw 'Python 3.12+ is required. Install Python or pass -PythonPath C:/path/python.exe.'}
 $arguments=@('-B',(Join-Path $PSScriptRoot 'server.py'),'--port',"$Port")
 if($Board){$arguments+=@('--board',$Board)}
 if(!$NoBrowser){$arguments+='--open'}
 Write-Host 'AX7021 local monitor. Keep this window open; press Ctrl+C to stop.'
 & $PythonPath @arguments
 exit $LASTEXITCODE
}catch{
 Write-Host $_.Exception.Message -ForegroundColor Red
 exit 1
}
