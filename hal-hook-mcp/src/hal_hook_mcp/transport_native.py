"""Native (on-device) transport — runs AS a process on the phone itself, talks
to the daemon via its unix socket directly, and reads/writes the ctrl_* files
as ordinary files on the same filesystem. No adb, no PC.

Expected deployment: AndroidHarness (which already bundles a Termux-style
Python 3.14 + libs under `/data/user/0/com.androidharness.app/files/linux/`)
launches this module as a subprocess under its root-granted profile. The
subprocess inherits SELinux context `u:r:ksu:s0` and uid 0, so:
  * `/data/local/tmp/hal_inject/daemon.sock` opens directly (mode 666 anyway,
    but the enclosing dir needs root to list).
  * `/data/local/tmp/hal_inject/ctrl_*` reads/writes work directly.
  * `/system/bin/logcat` runs with full visibility.
"""
from __future__ import annotations

import logging
import os
import re
import shutil
import socket
import subprocess
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

from .parsers import Event, parse_line

log = logging.getLogger(__name__)

DAEMON_SOCK = "/data/local/tmp/hal_inject/daemon.sock"
CTRL_DIR = "/data/local/tmp/hal_inject"
LOGCAT = os.environ.get("HAL_HOOK_LOGCAT", "/system/bin/logcat")


# ─────────────────────── Daemon socket (AF_UNIX direct) ───────────────────

@dataclass(slots=True)
class DaemonResponse:
    raw: str
    ok: bool
    rest: str

    @classmethod
    def parse(cls, raw: str) -> "DaemonResponse":
        line = raw.strip().splitlines()[0] if raw.strip() else ""
        parts = line.split(maxsplit=1)
        ok = bool(parts) and parts[0] == "OK"
        rest = parts[1] if len(parts) > 1 else ""
        return cls(raw=raw, ok=ok, rest=rest)


def daemon_send(command: str, *, timeout: float = 10.0) -> DaemonResponse:
    """Open AF_UNIX, send command+newline, read one response line, close.
    Sub-millisecond latency; no shell exec, no fork, no adb RTT."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(DAEMON_SOCK)
        s.sendall((command + "\n").encode("utf-8"))
        # Daemon speaks one line per connection then closes — slurp until EOF.
        chunks: list[bytes] = []
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
            # Early-exit if we already have a newline — the daemon always
            # terminates its response with one and most responses are short.
            if b"\n" in chunk:
                break
    finally:
        try: s.close()
        except Exception: pass
    raw = b"".join(chunks).decode("utf-8", errors="replace")
    return DaemonResponse.parse(raw)


def check_daemon_up() -> tuple[bool, str]:
    """Health check: STATUS round-trip + socket presence."""
    if not os.path.exists(DAEMON_SOCK):
        return False, f"socket not present at {DAEMON_SOCK} — is the daemon running?"
    try:
        r = daemon_send("STATUS", timeout=3.0)
    except (ConnectionRefusedError, FileNotFoundError, PermissionError) as e:
        return False, f"socket connect failed: {type(e).__name__}: {e}"
    except Exception as e:
        return False, f"socket error: {type(e).__name__}: {e}"
    # The daemon's STATUS command replies with "STATUS {json}", not "OK".
    # Any reply we got means the socket + daemon are alive.
    if r.ok or r.raw.startswith("STATUS "):
        return True, "ok"
    return False, f"daemon up but reply unexpected: {r.raw[:200]}"


# ─────────────────────── File I/O on the live filesystem ──────────────────

def read_ctrl_file(name: str) -> str:
    """Read any ctrl_* (or other) file under CTRL_DIR. Empty string on missing."""
    path = os.path.join(CTRL_DIR, name)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read().strip()
    except (FileNotFoundError, PermissionError):
        return ""


def pull_file(remote: str, local: str, *, timeout: float = 30.0) -> None:
    """On-device the 'device' path IS the local path — just copy it."""
    os.makedirs(os.path.dirname(os.path.abspath(local)) or ".", exist_ok=True)
    shutil.copy(remote, local)


def push_file(local: str, remote: str, *, timeout: float = 30.0) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(remote)) or ".", exist_ok=True)
    shutil.copy(local, remote)


def chmod(path: str, mode: int = 0o644) -> None:
    try:
        os.chmod(path, mode)
    except (FileNotFoundError, PermissionError):
        pass


def adb_shell(cmd: str, *, as_root: bool = False, timeout: float = 15.0) -> str:
    """Shim for callers that want to run an arbitrary shell command. On-device
    we just use /system/bin/sh — no adb, no su wrapper (we're already root)."""
    # Keep the same sentinels as the ADB version so caller behavior is identical.
    try:
        r = subprocess.run(["/system/bin/sh", "-c", cmd],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise TimeoutError(f"shell timeout after {timeout}s: {cmd!r}")
    return (r.stdout or "") + (r.stderr or "")


# ─────────────────────── Logcat (direct subprocess) ───────────────────────

def logcat_dump(lines: int = 200, grep: Optional[str] = None) -> str:
    """Grab the tail — /system/bin/logcat -d -t <N>."""
    r = subprocess.run([LOGCAT, "-d", "-t", str(lines)],
                       capture_output=True, text=True, timeout=15.0)
    text = (r.stdout or "") + (r.stderr or "")
    if grep:
        text = "\n".join(l for l in text.splitlines() if _grep(l, grep))
    return text


def _grep(line: str, pattern: str) -> bool:
    try:
        return re.search(pattern, line) is not None
    except re.error:
        return pattern in line


def pgrep(pattern: str) -> list[int]:
    out = adb_shell(f"pgrep -f '{pattern}' 2>/dev/null || true", timeout=5.0)
    pids: list[int] = []
    for tok in out.split():
        try: pids.append(int(tok))
        except ValueError: pass
    return pids


def cameraserver_hook_status() -> dict:
    pids = pgrep("cameraserver")
    if not pids:
        return {"cameraserver_pid": None, "libcam_hook_maps": 0}
    pid = pids[0]
    try:
        with open(f"/proc/{pid}/maps") as f:
            cnt = sum(1 for ln in f if "libcam_hook" in ln)
    except (FileNotFoundError, PermissionError):
        cnt = 0
    return {"cameraserver_pid": pid, "libcam_hook_maps": cnt}


def logcat_stream(duration_s: float, *, grep: Optional[str] = None,
                  on_event: Optional[Callable[[Event], None]] = None) -> list[Event]:
    """Open /system/bin/logcat as a child, collect parsed cam_hook events for
    `duration_s` seconds."""
    cmd = [LOGCAT, "-v", "threadtime", "-T", "0"]
    collected: list[Event] = []
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, bufsize=1)
    killer: Optional[threading.Timer] = None
    try:
        killer = threading.Timer(duration_s + 0.5, proc.terminate)
        killer.daemon = True
        killer.start()
        assert proc.stdout is not None
        for line in proc.stdout:
            if grep and not _grep(line, grep):
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
