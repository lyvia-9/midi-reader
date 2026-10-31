@echo off
cd /d "%~dp0"
if "%~1"=="" (
  echo Drag one or more .mid files onto run.bat
  pause
  exit /b 1
)
if not exist ".venv\Scripts\python.exe" (
  echo Run setup.bat first.
  pause
  exit /b 1
)
for %%F in (%*) do (
  echo.
  echo ==== %%~nxF
  .venv\Scripts\python -m reader_proto run "%%~fF"
)
echo.
pause
