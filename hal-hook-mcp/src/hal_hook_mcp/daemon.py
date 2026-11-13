"""High-level wrappers around the daemon's command set. Mirrors DaemonBridge.kt
in the app so the MCP surface and the on-device UI do the same things.

Each function returns a plain dict suitable for JSON serialization by the MCP
layer. Low-level `DaemonResponse` is hidden here."""
from __future__ import annotations

import json
from typing import Optional

from .transport import adb_shell, daemon_send, DaemonResponse, CTRL_DIR


def _ok(cmd: str) -> dict:
    r = daemon_send(cmd)
    return {"ok": r.ok, "raw": r.raw, "command": cmd}


# ── Capture / injection control ──────────────────────────────────────────────

def status() -> dict:
    """Pull the daemon's STATUS JSON plus the ctrl-file snapshot."""
    r = daemon_send("STATUS")
    daemon_json = None
    if r.raw.startswith("STATUS "):
        try:
            daemon_json = json.loads(r.raw[len("STATUS "):].strip())
        except Exception:
            daemon_json = {"_parse_error": True, "raw": r.raw}
    ctrl = ctrl_files()
    return {"daemon": daemon_json, "ctrl": ctrl, "raw": r.raw}


def ctrl_files() -> dict:
    """Read every ctrl_* file off the device in one shell round-trip."""
    cmd = (
        f"for f in {CTRL_DIR}/ctrl {CTRL_DIR}/ctrl_yuv {CTRL_DIR}/ctrl_cam "
        f"{CTRL_DIR}/cur_cam {CTRL_DIR}/ctrl_slot {CTRL_DIR}/ctrl_fmt "
        f"{CTRL_DIR}/ctrl_noise {CTRL_DIR}/ctrl_composite {CTRL_DIR}/ctrl_region "
        f"{CTRL_DIR}/ctrl_content_pad {CTRL_DIR}/ctrl_blob_res_w {CTRL_DIR}/ctrl_blob_res_h; "
        f"do printf \"%s=%s\\n\" \"$(basename $f)\" \"$(cat $f 2>/dev/null)\"; done"
    )
    out = adb_shell(cmd, as_root=True)
    result: dict = {}
    for ln in out.splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            result[k.strip()] = v.strip()
    return result


def start() -> dict:              return _ok("START")
def stop() -> dict:               return _ok("STOP")
def set_cam(mode: str) -> dict:   return _ok(f"SET_CAM {mode}")
def set_slot(slot: str) -> dict:  return _ok(f"SET_SLOT {slot}")
def set_fmt(fmt: str) -> dict:    return _ok(f"SET_FMT {fmt}")
def set_yuv_width(w: int) -> dict: return _ok(f"SET_YUV_WIDTH {int(w)}")
def set_composite(on: bool) -> dict: return _ok(f"SET_COMPOSITE {1 if on else 0}")
def set_region(cx: float, cy: float, w: float, h: float) -> dict:
    return _ok(f"SET_REGION {cx} {cy} {w} {h}")
def set_region_auto(cx: float, cy: float, size: float) -> dict:
    return _ok(f"SET_REGION {cx} {cy} {size}")
def set_content_pad(pct: int) -> dict:
    return _ok(f"SET_CONTENT_PAD {max(0, min(200, int(pct)))}")
def set_blob_res(w: int, h: int) -> dict:
    return _ok(f"SET_BLOB_RES {max(0, int(w))} {max(0, int(h))}")
def set_image(path: str) -> dict: return _ok(f"SET_IMAGE {path}")
def set_slot_image(slot: str, yuv_path: str, blob_path: str) -> dict:
    return _ok(f"SET_SLOT_IMAGE {slot} {yuv_path} {blob_path}")


def snap_yuv(dst_path: str) -> dict:
    """Trigger a pre-composite real-YUV snapshot. The daemon copies the file
    to `dst_path` (must be writable by root, which is anywhere). Returns the
    dims string, or an error."""
    r = daemon_send(f"SNAP_YUV {dst_path}", timeout=5.0)
    if not r.ok:
        return {"ok": False, "raw": r.raw}
    rest = r.rest
    stale = rest.startswith("STALE ")
    dims = rest[len("STALE "):].strip() if stale else rest.strip()
    return {"ok": True, "dims": dims, "stale": stale, "path": dst_path, "raw": r.raw}


def back_node() -> dict:
    r = daemon_send("BACK_NODE")
    return {"ok": True, "node": r.raw.strip()}
