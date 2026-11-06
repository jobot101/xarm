"""mcp_arm.py — Shop MotherShip arm tools for a local LLM (MCP over stdio, e.g. LM Studio's mcp.json).

Safety lives here, not in the model's prompt:
  - every target is clamped to the joint limits the board reports (Workshop limits), and a move may shift a joint
    by at most MAX_STEP units, so the model can't swing the arm across the bench in one call
  - travel time has a floor (MIN_MS_PER_100 per 100 units of the biggest joint change); with someone within
    NEAR_CM (radar) every move takes twice as long
  - nothing runs while the arm is busy with a job; `stop` always works
  - no torque-off tool: going limp drops the arm
Talks to the board's HTTP API (standard library only). Board address: ../johnny.json or ../board_ip.txt.
"""
from __future__ import annotations

import json
import os
import socket
import time
import urllib.request

from mcp.server.fastmcp import FastMCP

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
JOINTS = ["gripper", "wrist_twist", "wrist", "elbow", "shoulder", "base"]   # servo index i = bus ID i+1
MAX_STEP = 300            # biggest change per joint per move_joints call (positions run 0-1000)
MIN_MS_PER_100 = 250      # at least 0.25 s per 100 units of the largest change
NEAR_CM = 50              # someone this close: everything at half speed

mcp = FastMCP("mothership")


def _board() -> str:
    try: host = json.load(open(os.path.join(ROOT, "johnny.json"))).get("board", "shop-mothership.local")
    except (OSError, ValueError): host = "shop-mothership.local"
    try: return socket.gethostbyname(host)
    except OSError:
        try: return open(os.path.join(ROOT, "board_ip.txt")).read().strip() or host
        except OSError: return host


BOARD = _board()


def _call(path: str, body: dict | None = None, timeout: float = 10) -> dict:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f"http://{BOARD}{path}", data=data, method="POST" if data else "GET",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read() or b"{}")


def _state() -> dict: return _call("/api/state")


def _near(st: dict) -> bool:
    r = st.get("radar") or {}
    return bool(r.get("present")) and 0 <= (r.get("distance_cm") if r.get("distance_cm") is not None else -1) < NEAR_CM


def _move(target: list[int], seconds: float | None) -> str:
    st = _state()
    if st.get("busy"): return "REFUSED: the arm is busy with a job. Wait, or call stop."
    arm = st["arm"]
    if not arm.get("torque"): return "REFUSED: the arm is limp (motors off). Someone has to press Hold first."
    now, lim = arm["pos"], arm.get("lim") or [[0, 1000]] * 6
    goal, notes = [], []
    for i, (cur, want) in enumerate(zip(now, target)):
        lo, hi = lim[i]
        w = max(lo, min(hi, int(want)))
        if w != int(want): notes.append(f"{JOINTS[i]} kept inside its limits {lo}-{hi}")
        if abs(w - cur) > MAX_STEP:
            w = cur + MAX_STEP * (1 if w > cur else -1)
            notes.append(f"{JOINTS[i]} limited to {MAX_STEP} units this move")
        goal.append(w)
    biggest = max(abs(g - c) for g, c in zip(goal, now))
    if biggest == 0: return "already there"
    ms = max(int(biggest * MIN_MS_PER_100 / 100), 300, int((seconds or 0) * 1000))
    if _near(st): ms *= 2; notes.append("someone is close: moving at half speed")
    r = _call("/api/jog", {"pos": goal, "ms": ms})
    if not r.get("ok"): return f"board refused: {r.get('msg')}"
    time.sleep(ms / 1000 + 0.15)
    after = _state()["arm"]["pos"]
    return json.dumps({"moved_to": dict(zip(JOINTS, after)), "took_ms": ms, "notes": notes})


@mcp.tool()
def arm_state() -> str:
    """Where the arm is right now. Joint positions run 0-1000 (500 is the middle): gripper (low = open,
    high = closed), wrist_twist, wrist, elbow, shoulder (lower numbers lift the arm), base (turns left/right).
    Also each joint's safe limits, whether it is busy or holding something, and whether someone is nearby."""
    st = _state()
    arm, r = st["arm"], st.get("radar") or {}
    return json.dumps({
        "joints": dict(zip(JOINTS, arm["pos"])),
        "limits": dict(zip(JOINTS, arm.get("lim") or [])),
        "busy": st.get("busy"), "state": st.get("state"), "detail": st.get("detail"),
        "holding": st.get("holding"), "motors_on": arm.get("torque"),
        "person": {"present": r.get("present"), "distance_cm": r.get("distance_cm"), "angle_deg": r.get("angle_deg")},
    })


@mcp.tool()
def move_joints(gripper: int | None = None, wrist_twist: int | None = None, wrist: int | None = None,
                elbow: int | None = None, shoulder: int | None = None, base: int | None = None,
                seconds: float | None = None) -> str:
    """Move one or more joints to absolute positions (0-1000). Leave a joint out to keep it where it is.
    Each joint moves at most 300 units per call, stays inside its limits, and the move takes at least
    0.25 s per 100 units (longer if someone is close). Call arm_state first to see where it is."""
    now = _state()["arm"]["pos"]
    want = [gripper, wrist_twist, wrist, elbow, shoulder, base]
    return _move([w if w is not None else c for w, c in zip(want, now)], seconds)


@mcp.tool()
def go_to_pose(name: str) -> str:
    """Go to a taught pose: home, drop, give (hand-off spot), helper ("hold this" spot) or bell (chime strike point).
    Long moves are made in safe steps of at most 300 units per joint."""
    cfg = _call("/api/config")
    poses = cfg.get("poses") or {k: cfg[k] for k in ("home", "drop") if k in cfg}   # older firmware: home + drop only
    key = {"home": "home", "drop": "drop", "give": "give", "hand-off": "give", "handoff": "give",
           "helper": "helper", "bell": "bell", "chime": "bell"}.get(name.strip().lower())
    if not key or not poses.get(key): return f"no taught pose called {name!r} here (this board knows: {', '.join(poses)})"
    target, out = list(poses[key]), []
    for _ in range(4):                               # up to 4 steps of MAX_STEP
        res = _move(target, None)
        out.append(res)
        if res == "already there" or res.startswith(("REFUSED", "board refused")): break
        if max(abs(a - b) for a, b in zip(_state()["arm"]["pos"], target)) < 8: break
    return out[-1]


@mcp.tool()
def gripper(action: str = "open", position: int | None = None) -> str:
    """Open or close the gripper, or set it to a position (0-1000, low = open). action: open | close | set."""
    if action in ("open", "close") and position is None:
        r = _call("/api/cmd", {"text": "open your hand" if action == "open" else "close your hand"})
        return r.get("msg", "ok")
    return move_joints(gripper=position if position is not None else 500)


@mcp.tool()
def nudge(direction: str) -> str:
    """A small, safe step: left, right, up, down, tilt up, tilt down, turn left, turn right."""
    d = direction.strip().lower()
    phrase = {"left": "a little left", "right": "a little right", "up": "a little up", "down": "a little down",
              "tilt up": "tilt up", "tilt down": "tilt down", "turn left": "turn left", "turn right": "turn right"}.get(d)
    if not phrase: return "unknown direction (left, right, up, down, tilt up, tilt down, turn left, turn right)"
    return _call("/api/cmd", {"text": phrase}).get("msg", "ok")


@mcp.tool()
def run_command(text: str) -> str:
    """Run one of MotherShip's own commands, for example: ring the bell, hold this for me, grab it, let go,
    give it to me, put it down, go home, wave hello, take a bow, dance robot, do the <trick name>."""
    r = _call("/api/cmd", {"text": text})
    return f"{'ok' if r.get('ok') else 'not done'}: {r.get('msg', '')}"


@mcp.tool()
def ring_chime(pattern: str = "xxx", strength: int = 50) -> str:
    """Ring the chime with the arm. pattern: x strike, ! accent, o soft, . rest (up to 32). strength 10-100."""
    r = _call("/api/chime", {"pattern": pattern, "strength": max(10, min(100, int(strength)))})
    return r.get("msg", "ok") if r.get("ok") else f"not done: {r.get('msg')}"


@mcp.tool()
def stop() -> str:
    """Stop the arm immediately (always allowed)."""
    return _call("/api/cmd", {"text": "stop"}).get("msg", "stopped")


if __name__ == "__main__":
    mcp.run()
