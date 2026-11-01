@echo off
setlocal
if exist "%~dp0runtime\python.exe" goto portable
if not exist "%~dp0start_monitor.ps1" goto incomplete
powershell.exe -NoProfile -File "%~dp0start_monitor.ps1" %*
if errorlevel 1 pause
exit /b
:portable
set "entry=%~dp0runtime"
set "log=%TEMP%\AX7021Monitor-startup.log"
if not exist "%entry%\desktop.py" goto incomplete
if not exist "%entry%\python313.stdlib" goto incomplete
if not exist "%entry%\pythonw.exe" goto incomplete
"%entry%\python.exe" -I -B -c "import desktop, encodings, http.server" >"%log%" 2>&1
if not "%errorlevel%"=="0" goto failed
if /I "%~1"=="--check" exit /b 0
if /I "%~1"=="--browser" goto browser
if not exist "%~dp0AX7021Monitor.exe" goto incomplete
start "" "%~dp0AX7021Monitor.exe"
if errorlevel 1 goto failed
exit /b 0
:browser
start "" "%entry%\pythonw.exe" -I -B "%entry%\desktop.py"
if errorlevel 1 goto failed
exit /b 0
:incomplete
echo [AX7021Monitor] Missing runtime files. Extract the entire download to a new folder.
goto stop
:failed
echo [AX7021Monitor] Could not start the included runtime. See "%log%".
if exist "%log%" type "%log%"
:stop
if /I not "%~1"=="--check" pause
exit /b 1
