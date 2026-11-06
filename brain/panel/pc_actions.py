"""pc_actions.py — a fixed menu of things MotherShip may run on the shop PC.

The menu IS the folder `../pc_actions/` (next to brain.json). Every .bat, .ps1 or .py in it is one action:
  - its name is the file name without the extension, e.g. `take a snapshot.bat` -> "take a snapshot"
  - its first comment line (`rem ...`, `# ...`, `:: ...`) is the description the model reads
Nothing outside that folder can run: the model picks a name, this module finds the file, and that's the only
path to the shell. A run is capped at RUN_SECS and its output trimmed to a few lines.

    python pc_actions.py                 # list the menu
    python pc_actions.py run "take a snapshot"
"""
from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MENU = os.path.join(ROOT, "pc_actions")
RUN_SECS = 120
EXTS = (".bat", ".cmd", ".ps1", ".py")
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def _describe(path: str) -> str:
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for _ in range(5):
                line = f.readline().strip()
                for lead in ("rem ", "REM ", ":: ", "# ", "#", "@rem "):
                    if line.startswith(lead): return line[len(lead):].strip()
    except OSError:
        pass
    return ""


def menu() -> dict[str, dict]:
    """name -> {file, description}"""
    out = {}
    if not os.path.isdir(MENU): return out
    for fn in sorted(os.listdir(MENU)):
        base, ext = os.path.splitext(fn)
        if ext.lower() not in EXTS or base.startswith("_"): continue
        out[base.lower()] = {"file": os.path.join(MENU, fn), "description": _describe(os.path.join(MENU, fn))}
    return out


def find(name: str) -> dict | None:
    m, key = menu(), name.strip().lower()
    if key in m: return m[key]
    hits = [v for k, v in m.items() if key and key in k]
    return hits[0] if len(hits) == 1 else None


def run(name: str) -> str:
    a = find(name)
    if not a: return f"no PC action called {name!r} (menu: {', '.join(menu()) or 'empty'})"
    f = a["file"]
    if os.path.commonpath([os.path.realpath(f), os.path.realpath(MENU)]) != os.path.realpath(MENU):
        return "refused: outside the menu folder"
    ext = os.path.splitext(f)[1].lower()
    if ext in (".bat", ".cmd"): cmd = ["cmd.exe", "/c", f]
    elif ext == ".ps1": cmd = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", f]
    else: cmd = [sys.executable, f]
    try:
        p = subprocess.run(cmd, cwd=MENU, capture_output=True, text=True, timeout=RUN_SECS, creationflags=NO_WINDOW)
    except subprocess.TimeoutExpired:
        return f"{os.path.basename(f)}: still running after {RUN_SECS} s, gave up waiting"
    out = (p.stdout or "").strip().splitlines()[-6:]
    err = (p.stderr or "").strip().splitlines()[-3:]
    text = "\n".join(out + ([f"[stderr] {l}" for l in err] if p.returncode else []))
    return f"{os.path.basename(f)}: {'done' if p.returncode == 0 else f'failed (exit {p.returncode})'}" + (f"\n{text}" if text else "")


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "run": print(run(" ".join(sys.argv[2:])))
    else:
        for k, v in menu().items(): print(f"{k:28} {v['description']}")
