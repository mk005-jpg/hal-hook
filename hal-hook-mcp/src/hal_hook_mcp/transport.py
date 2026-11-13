"""Transport dispatcher — picks the right backend at import time.

Auto-selects by checking whether the hal daemon's unix socket is directly
reachable at the well-known path. If yes, we're ON the device (AH chroot,
Termux, KSU module) and should use the native socket + direct file I/O
backend. If no, fall back to adb-over-shell which drives a remote device
from a host workstation.

Override: `HAL_HOOK_TRANSPORT=adb` or `HAL_HOOK_TRANSPORT=native` forces a
specific backend (useful for testing, or if auto-detect misfires in a weird
chroot where daemon.sock is bind-mounted but the running process isn't
actually root).

Both backends export the same symbols, so downstream modules just do
`from .transport import daemon_send, read_ctrl_file, pull_file, …` and
never care which path is active.
"""
from __future__ import annotations

import logging
import os

log = logging.getLogger(__name__)

_FORCE = os.environ.get("HAL_HOOK_TRANSPORT", "").lower().strip()
_NATIVE_SOCK = "/data/local/tmp/hal_inject/daemon.sock"


def _pick() -> str:
    if _FORCE in ("adb", "native"):
        return _FORCE
    # Auto-detect: if the socket exists as a local file AND we can at least stat
    # it, assume native. In the pathological case where someone sets up a bind
    # mount on their PC filesystem we'd be wrong, but that's extremely rare —
    # set HAL_HOOK_TRANSPORT=adb to override.
    try:
        st = os.stat(_NATIVE_SOCK)
        return "native"
    except (FileNotFoundError, PermissionError, OSError):
        return "adb"


BACKEND = _pick()
log.info("hal-hook-mcp transport backend: %s", BACKEND)

if BACKEND == "native":
    from .transport_native import (            # noqa: F401
        DAEMON_SOCK, CTRL_DIR, DaemonResponse,
        daemon_send, check_daemon_up,
        read_ctrl_file, pull_file, push_file, chmod,
        adb_shell,
        logcat_dump, logcat_stream, pgrep, cameraserver_hook_status,
    )
else:
    from .transport_adb import (               # noqa: F401
        DAEMON_SOCK, CTRL_DIR, DaemonResponse,
        daemon_send, check_daemon_up,
        read_ctrl_file, pull_file, push_file, chmod,
        adb_shell, adb_pull, adb_push,
        logcat_dump, logcat_stream, pgrep, cameraserver_hook_status,
    )
