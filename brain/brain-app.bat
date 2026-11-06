@echo off
rem The brain's desktop app: camera view, bell tuning, mics, commands. Uses the same Python environment as run-brain.bat.
cd /d "%~dp0"
if not exist .venv\Scripts\pythonw.exe (
  echo Run run-brain.bat once first: it sets up the brain's Python environment.
  pause & exit /b 1
)
if not exist .venv\Lib\site-packages\PIL .venv\Scripts\python -m pip install pillow
start "" .venv\Scripts\pythonw.exe brain_app.py %*
