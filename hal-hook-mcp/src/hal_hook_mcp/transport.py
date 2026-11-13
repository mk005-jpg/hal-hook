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
    # STATUS is the daemon's ping — returns JSON if alive.
    r = daemon_send("STATUS", timeout=5.0)
    if r.ok:
        return True, "ok"
    # Try to tell WHY it failed.
    ps = adb_shell("ps -A 2>/dev/null | grep -E 'system_health_check|hal_inject_daemon' || true",
                   as_root=True, timeout=5.0)
    if not ps.strip():
        return False, "daemon process not running on device"
    sock = adb_shell(f"ls -la {DAEMON_SOCK} 2>&1", as_root=True, timeout=5.0)
    return False, f"daemon up but socket unreachable; sock={sock.strip()}; ps={ps.strip()}"
