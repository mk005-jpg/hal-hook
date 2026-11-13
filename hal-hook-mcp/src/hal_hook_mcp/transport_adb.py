"""Transport layer — talks to the hal-inject daemon's unix socket via adb.

Every call goes:  host-python -> adb shell -> su -> nc -U -> daemon.sock

Pros: zero device-side setup; works through any adb-reachable path (USB, wifi,
tcp bridge). Cons: ~60-120 ms per call. For streaming (logcat tail, watch
captures) we use `adb logcat` + `subprocess.Popen` directly, not nc.

An ADB_SERIAL env var pins a specific device when several are attached.
"""
from __future__ import annotations

import logging
import os
import shlex
import shutil
import subprocess
from dataclasses import dataclass

log = logging.getLogger(__name__)

DAEMON_SOCK = "/data/local/tmp/hal_inject/daemon.sock"
CTRL_DIR = "/data/local/tmp/hal_inject"


def _adb_cmd() -> list[str]:
    """adb binary + optional serial. Honors $ADB (path to binary) and
    $ADB_SERIAL (device id). Falls back to `adb` on PATH."""
    adb = os.environ.get("ADB") or shutil.which("adb") or "adb"
    serial = os.environ.get("ADB_SERIAL")
    base = [adb]
    if serial:
        base += ["-s", serial]
    return base


def adb_shell(cmd: str, *, as_root: bool = False, timeout: float = 15.0) -> str:
    """Run a shell command on the device and return stdout (stderr merged).

    When `as_root` is True, the command is wrapped in `su -c '...'`. The caller
    must pre-escape single quotes inside `cmd` when as_root=True because we
    don't try to bash-escape here (callers always pass trusted strings).
    """
    full = _adb_cmd() + ["shell"]
    if as_root:
        full.append(f"su -c '{cmd}'")
    else:
        full.append(cmd)
    log.debug("adb %s", shlex.join(full))
    try:
        r = subprocess.run(full, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise TimeoutError(f"adb shell timeout after {timeout}s: {cmd!r}")
    return (r.stdout or "") + (r.stderr or "")


def adb_pull(remote: str, local: str, *, timeout: float = 30.0) -> None:
    """Pull a device-side file to the host. The remote path must be world-
    readable (mode 0644) OR you must first cp to /sdcard; this helper does
    not sudo."""
    # MSYS_NO_PATHCONV=1 avoids git-bash on Windows mangling /data paths.
    env = {**os.environ, "MSYS_NO_PATHCONV": "1"}
    full = _adb_cmd() + ["pull", remote, local]
    subprocess.run(full, env=env, capture_output=True, text=True,
                   timeout=timeout, check=True)


def adb_push(local: str, remote: str, *, timeout: float = 30.0) -> None:
    env = {**os.environ, "MSYS_NO_PATHCONV": "1"}
    full = _adb_cmd() + ["push", local, remote]
    subprocess.run(full, env=env, capture_output=True, text=True,
                   timeout=timeout, check=True)


@dataclass(slots=True)
class DaemonResponse:
    """Parsed daemon reply. `ok` is True iff the first token is 'OK'."""
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
    """Send a single command+newline to the daemon via nc-over-adb-shell and
    return the first reply line parsed. The daemon speaks a line-oriented
    protocol: one command per connection, immediate response, close."""
    # The command is embedded inside su -c '...' so single-quotes in it would
    # break the quoting. The daemon's command set doesn't use single quotes,
    # so we just reject them defensively.
    if "'" in command:
        raise ValueError("daemon command must not contain single quotes")
    # Use -w 2 so nc gives up after 2 s instead of hanging on a dead socket.
    # printf %s avoids a trailing newline injection from echo -e inconsistencies.
    shell = f"printf '%s\\n' \"{command}\" | nc -w 2 -U {DAEMON_SOCK}"
    raw = adb_shell(shell, as_root=True, timeout=timeout)
    return DaemonResponse.parse(raw)


def check_daemon_up() -> tuple[bool, str]:
    """Quick health check. Returns (alive, diagnostic_message)."""
    # STATUS is the daemon's ping — it answers with "STATUS {json}", NOT with
    # "OK …", so we accept either prefix as "socket + daemon alive".
    r = daemon_send("STATUS", timeout=5.0)
    if r.ok or r.raw.startswith("STATUS "):
        return True, "ok"
    # Try to tell WHY it failed.
    ps = adb_shell("ps -A 2>/dev/null | grep -E 'system_health_check|hal_inject_daemon' || true",
                   as_root=True, timeout=5.0)
    if not ps.strip():
        return False, "daemon process not running on device"
    sock = adb_shell(f"ls -la {DAEMON_SOCK} 2>&1", as_root=True, timeout=5.0)
    return False, f"daemon up but socket unreachable; sock={sock.strip()}; ps={ps.strip()}"


# ─── Uniform helpers shared with the on-device backend ────────────────────
# Both transport_adb and transport_native expose the SAME names so the
# dispatcher module can swap backends without changing downstream code.

def read_ctrl_file(name: str) -> str:
    out = adb_shell(f"cat {CTRL_DIR}/{name} 2>/dev/null", as_root=True, timeout=5.0)
    return out.strip()


def pull_file(remote: str, local: str, *, timeout: float = 30.0) -> None:
    adb_pull(remote, local, timeout=timeout)


def push_file(local: str, remote: str, *, timeout: float = 30.0) -> None:
    adb_push(local, remote, timeout=timeout)


def chmod(path: str, mode: int = 0o644) -> None:
    adb_shell(f"chmod {mode:o} {path} 2>/dev/null || true",
              as_root=True, timeout=5.0)


# Logcat helpers (previously in monitor.py — moved here so native + adb
# backends present the same surface).

def logcat_dump(lines: int = 200, grep: Optional[str] = None) -> str:
    import subprocess as _sp
    cmd = _adb_cmd() + ["logcat", "-d", "-t", str(lines)]
    r = _sp.run(cmd, capture_output=True, text=True, timeout=15.0)
    text = (r.stdout or "") + (r.stderr or "")
    if grep:
        text = "\n".join(l for l in text.splitlines() if _grep(l, grep))
    return text


def _grep(line: str, pattern: str) -> bool:
    import re
    try: return re.search(pattern, line) is not None
    except re.error: return pattern in line


def pgrep(pattern: str) -> list[int]:
    out = adb_shell(f"pgrep -f '{pattern}' 2>/dev/null || true",
                    as_root=True, timeout=5.0)
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
    out = adb_shell(f"grep -c libcam_hook /proc/{pid}/maps 2>/dev/null || echo 0",
                    as_root=True, timeout=5.0)
    try: cnt = int(out.strip().splitlines()[-1])
    except (ValueError, IndexError): cnt = 0
    return {"cameraserver_pid": pid, "libcam_hook_maps": cnt}


def logcat_stream(duration_s: float, *, grep: Optional[str] = None,
                  on_event=None) -> list:
    import subprocess as _sp
    import threading as _th
    import time as _t
    from .parsers import parse_line, Event
    cmd = _adb_cmd() + ["logcat", "-v", "threadtime", "-T", "0"]
    collected: list[Event] = []
    start = _t.monotonic()
    proc = _sp.Popen(cmd, stdout=_sp.PIPE, stderr=_sp.DEVNULL, text=True,
                     bufsize=1, env={**os.environ, "MSYS_NO_PATHCONV": "1"})
    killer: Optional[_th.Timer] = None
    try:
        killer = _th.Timer(duration_s + 0.5, proc.terminate)
        killer.daemon = True; killer.start()
        assert proc.stdout is not None
        for line in proc.stdout:
            if grep and not _grep(line, grep): continue
            ev = parse_line(line)
            if ev is None: continue
            collected.append(ev)
            if on_event:
                try: on_event(ev)
                except Exception: pass
            if _t.monotonic() - start >= duration_s: break
    finally:
        if killer: killer.cancel()
        try: proc.terminate()
        except Exception: pass
        try: proc.wait(timeout=1.0)
        except Exception:
            try: proc.kill()
            except Exception: pass
    return collected


# Needed above
from typing import Optional
