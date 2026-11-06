@echo off
rem Johnny's PC brain. First run sets up a Python environment here (a few minutes, downloads Whisper).
rem Needs: LM Studio running with its local server on (Developer tab -> Start Server, port 1234) and a model loaded.
cd /d "%~dp0"
if not exist .venv (
  echo Setting up Johnny...
  py -3 -m venv .venv || python -m venv .venv
  .venv\Scripts\python -m pip install --upgrade pip
  .venv\Scripts\pip install -r requirements.txt
)
.venv\Scripts\python johnny.py %*
pause
