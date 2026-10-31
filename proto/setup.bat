@echo off
cd /d "%~dp0"
where py >nul 2>nul && (set "PY=py -3") || (set "PY=python")
echo Creating virtual environment...
%PY% -m venv .venv || goto :fail
.venv\Scripts\python -m pip install --upgrade pip
.venv\Scripts\python -m pip install symusic mido pytest || goto :fail
echo.
echo Setup done. Now drag a .mid file onto run.bat
pause
exit /b 0
:fail
echo.
echo Setup failed. Is Python 3.10 or newer installed? https://www.python.org/downloads/
pause
exit /b 1
