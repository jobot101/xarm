@echo off
rem Johnny's desktop app: camera view, bell tuning, mics, commands. Uses the same Python environment as run-johnny.bat.
cd /d "%~dp0"
if not exist .venv\Scripts\pythonw.exe (
  echo Run run-johnny.bat once first: it sets up Johnny's Python environment.
  pause & exit /b 1
)
if not exist .venv\Lib\site-packages\PIL .venv\Scripts\pip install pillow
start "" .venv\Scripts\pythonw.exe johnny_app.py %*
