# Save a picture from the PC webcam (the OBSBOT, else the bench webcam) to the Shots folder (and say where it went).
import datetime, os, subprocess, sys
out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "shots")
os.makedirs(out_dir, exist_ok=True)
f = os.path.join(out_dir, datetime.datetime.now().strftime("shot-%Y%m%d-%H%M%S.jpg"))
look = os.path.join(os.path.dirname(out_dir), "look.py")
py = os.path.join(os.path.dirname(out_dir), ".venv", "Scripts", "python.exe")
r = subprocess.run([py, look, "snap", f], capture_output=True, text=True, timeout=30)
if os.path.exists(f): print(f"saved {f}")
elif "I/O error" in (r.stderr or ""): print("no webcam free: check one is plugged in, close OBSBOT Center (or whatever has it) and try again")
else: print("no picture: " + (r.stderr or r.stdout).strip().splitlines()[-1][:120])
