"""Live-monitoring helpers — adb logcat subscriptions, process lookups, etc.

These wrap `adb logcat -d` / `-t <N>` (dump recent) and `adb logcat` with a
Popen pipe for streaming. The streaming forms are used only from the
`hal_watch_captures` and `hal_probe_target` tools which bound the duration."""
from __future__ import annotations

import os
import subprocess
import threading
import time
from typing import Callable, Iterable, Optional

from .parsers import Event, parse_line
from .transport import _adb_cmd, adb_shell


def logcat_dump(lines: int = 200, grep: Optional[str] = None) -> str:
    """Grab the tail of logcat. `grep` is passed literally to `grep -E`."""
    cmd = _adb_cmd() + ["logcat", "-d", "-t", str(lines)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=15.0)
    text = (r.stdout or "") + (r.stderr or "")
    if grep:
        # Simple python-side filter so we don't pay for a device-side pipe.
        text = "\n".join(l for l in text.splitlines() if _re_grep(l, grep))
    return text


def _re_grep(line: str, pattern: str) -> bool:
    import re
    try:
        return re.search(pattern, line) is not None
    except re.error:
        return pattern in line


def pgrep(pattern: str) -> list[int]:
    """Return matching process PIDs on the device (may need root for some)."""
    out = adb_shell(f"pgrep -f '{pattern}' 2>/dev/null || true",
                    as_root=True, timeout=5.0)
    pids: list[int] = []
    for tok in out.split():
        try:
            pids.append(int(tok))
        except ValueError:
            pass
    return pids


def cameraserver_hook_status() -> dict:
    """Is the hook mapped into cameraserver right now? Useful for `hal_status`."""
    pids = pgrep("cameraserver")
    if not pids:
        return {"cameraserver_pid": None, "libcam_hook_maps": 0}
    pid = pids[0]
    out = adb_shell(f"grep -c libcam_hook /proc/{pid}/maps 2>/dev/null || echo 0",
                    as_root=True, timeout=5.0)
    try:
        cnt = int(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        cnt = 0
    return {"cameraserver_pid": pid, "libcam_hook_maps": cnt}


def logcat_stream(duration_s: float, *, grep: Optional[str] = None,
                  on_event: Optional[Callable[[Event], None]] = None) -> list[Event]:
    """Open `adb logcat -v threadtime` and collect parsed cam_hook events for
    `duration_s` seconds. Blocking; caller must run in its own thread if they
    want to stay responsive. Returns the collected events; also calls
    `on_event(ev)` for each as they arrive (lets callers stream)."""
    cmd = _adb_cmd() + ["logcat", "-v", "threadtime", "-T", "0"]
    # -T 0 starts from "now" so we don't flood with the backlog.
    collected: list[Event] = []
    start = time.monotonic()
    # bufsize=1 = line-buffered on text streams.
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, bufsize=1,
                            env={**os.environ, "MSYS_NO_PATHCONV": "1"})
    killer: Optional[threading.Timer] = None
    try:
        # Hard-kill after deadline so a quiet stream doesn't hang us forever.
        killer = threading.Timer(duration_s + 0.5, proc.terminate)
        killer.daemon = True
        killer.start()
        assert proc.stdout is not None
        for line in proc.stdout:
            if grep and not _re_grep(line, grep):
                continue
            ev = parse_line(line)
            if ev is None:
                continue
            collected.append(ev)
            if on_event:
                try: on_event(ev)
                except Exception: pass
            if time.monotonic() - start >= duration_s:
                break
    finally:
        if killer: killer.cancel()
        try: proc.terminate()
        except Exception: pass
        try: proc.wait(timeout=1.0)
        except Exception:
            try: proc.kill()
            except Exception: pass
    return collected
