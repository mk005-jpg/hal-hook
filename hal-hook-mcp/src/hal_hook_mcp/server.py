"""MCP server using the stdlib-only mini_mcp implementation.

Rewritten from the previous mcp-SDK-based server so the whole stack has zero
pip dependencies — the SDK's Rust-compiled transitive deps (pydantic-core,
rpds-py, cryptography) do not build on aarch64-android, which was blocking
the on-device install.

Tools + resources are identical to the prior SDK version; only the transport
layer and registration decorators changed."""
from __future__ import annotations

import base64
import json
import logging
import os
import tempfile
from pathlib import Path

from . import daemon, probe
from .mini_mcp import Server
from .transport import (
    adb_shell, chmod, check_daemon_up, logcat_dump, logcat_stream,
    pull_file, push_file, CTRL_DIR,
)

log = logging.getLogger(__name__)

app = Server("hal-hook-mcp", version="0.2.0")

# ── Control tools ──────────────────────────────────────────────────────────

@app.tool("hal_status",
          "Snapshot of the hal-inject daemon state: STATUS JSON, every ctrl_* file, cameraserver PID + libcam_hook map count. Call first.",
          {"type": "object", "properties": {}, "required": []})
def _hal_status(a):
    up, msg = check_daemon_up()
    if not up:
        return {"ok": False, "error": msg}
    return daemon.status()


@app.tool("hal_start", "Arm the daemon (equivalent of the app's Start). Enables injection.",
          {"type": "object", "properties": {}, "required": []})
def _hal_start(a): return daemon.start()

@app.tool("hal_stop", "Disarm the daemon. Camera passes through unmodified.",
          {"type": "object", "properties": {}, "required": []})
def _hal_stop(a): return daemon.stop()


@app.tool("hal_set_cam", "Pick which camera facing to inject on.",
          {"type": "object",
           "properties": {"mode": {"type": "string", "enum": ["back", "front", "both"]}},
           "required": ["mode"]})
def _hal_set_cam(a): return daemon.set_cam(a["mode"])

@app.tool("hal_set_slot", "Activate a prepared media slot.",
          {"type": "object",
           "properties": {"slot": {"type": "string", "enum": ["idfront", "idback"]}},
           "required": ["slot"]})
def _hal_set_slot(a): return daemon.set_slot(a["slot"])

@app.tool("hal_set_fmt", "YUV chroma order for the injected slot.",
          {"type": "object",
           "properties": {"fmt": {"type": "string", "enum": ["nv21", "nv12"]}},
           "required": ["fmt"]})
def _hal_set_fmt(a): return daemon.set_fmt(a["fmt"])

@app.tool("hal_set_yuv_width", "Minimum YUV stream width to inject on (0 disables YUV inject).",
          {"type": "object",
           "properties": {"width": {"type": "integer", "minimum": 0}},
           "required": ["width"]})
def _hal_set_yuv_width(a): return daemon.set_yuv_width(a["width"])

@app.tool("hal_set_composite", "Toggle the Step-1 region compositor.",
          {"type": "object",
           "properties": {"on": {"type": "boolean"}},
           "required": ["on"]})
def _hal_set_composite(a): return daemon.set_composite(a["on"])

@app.tool("hal_set_region", "Where in the sensor frame the ID is painted. 0..1 normalized.",
          {"type": "object",
           "properties": {"cx": {"type": "number"}, "cy": {"type": "number"},
                          "w":  {"type": "number"}, "h":  {"type": "number"}},
           "required": ["cx", "cy", "w", "h"]})
def _hal_set_region(a):
    return daemon.set_region(a["cx"], a["cy"], a["w"], a["h"])

@app.tool("hal_set_content_pad",
          "Content-PAD strength 0..200 (fibre + laminate-specular pass vs 'digital screen' detectors).",
          {"type": "object",
           "properties": {"percent": {"type": "integer", "minimum": 0, "maximum": 200}},
           "required": ["percent"]})
def _hal_set_content_pad(a): return daemon.set_content_pad(a["percent"])

@app.tool("hal_set_blob_res",
          "Force BLOB rescale target. (0, 0) = AUTO: hook matches KYC-negotiated stream dims per call.",
          {"type": "object",
           "properties": {"w": {"type": "integer", "minimum": 0},
                          "h": {"type": "integer", "minimum": 0}},
           "required": ["w", "h"]})
def _hal_set_blob_res(a): return daemon.set_blob_res(a["w"], a["h"])


@app.tool("hal_push_slot_image",
          "Push a YUV and/or BLOB JPEG into a slot. On-device: paths are read directly. Over adb: pushed first.",
          {"type": "object",
           "properties": {"slot":       {"type": "string", "enum": ["idfront", "idback"]},
                          "local_yuv":  {"type": "string"},
                          "local_blob": {"type": "string"}},
           "required": ["slot"]})
def _hal_push_slot_image(a):
    slot = a["slot"]
    ly = a.get("local_yuv"); lb = a.get("local_blob")
    if not ly and not lb:
        return {"ok": False, "error": "need at least one of local_yuv, local_blob"}
    dev_cache = f"/data/local/tmp/hal_inject_push_{slot}"
    adb_shell(f"mkdir -p {dev_cache}", as_root=True, timeout=5)
    dev_yuv  = f"{dev_cache}/yuv.jpg"  if ly else ""
    dev_blob = f"{dev_cache}/blob.jpg" if lb else ""
    if ly: push_file(ly, dev_yuv)
    if lb: push_file(lb, dev_blob)
    adb_shell(f"chmod -R 644 {dev_cache}/*.jpg 2>/dev/null || true",
              as_root=True, timeout=5)
    r = daemon.set_slot_image(slot, dev_yuv or dev_blob, dev_blob or dev_yuv)
    return {**r, "pushed": {"yuv": dev_yuv, "blob": dev_blob}}


@app.tool("hal_snap_yuv",
          "Dump the next pre-composite real YUV frame (or the cached one) to <local_dst> and return dims.",
          {"type": "object",
           "properties": {"local_dst": {"type": "string"}},
           "required": ["local_dst"]})
def _hal_snap_yuv(a):
    local_dst = a["local_dst"]
    device_dst = "/data/local/tmp/hal_inject_mcp_snap.nv21"
    r = daemon.snap_yuv(device_dst)
    if not r.get("ok"):
        return r
    adb_shell(f"chmod 644 {device_dst} {device_dst.rsplit('.',1)[0]}.dim",
              as_root=True, timeout=5)
    os.makedirs(os.path.dirname(os.path.abspath(local_dst)) or ".", exist_ok=True)
    pull_file(device_dst, local_dst)
    pull_file(device_dst.rsplit(".", 1)[0] + ".dim", local_dst + ".dim")
    return {**r, "local_path": local_dst}


@app.tool("hal_pull_slot_blob",
          "Fetch the current frame_<slot>.jpg from the device; return dims + base64 preview (first 96 KiB).",
          {"type": "object",
           "properties": {"slot": {"type": "string", "enum": ["idfront", "idback"]}},
           "required": ["slot"]})
def _hal_pull_slot_blob(a):
    slot = a["slot"]
    remote = f"{CTRL_DIR}/frame_{slot}.jpg"
    with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as tf:
        local = tf.name
    adb_shell(f"chmod 644 {remote} 2>/dev/null || true",
              as_root=True, timeout=5)
    pull_file(remote, local)
    dims = None
    try:
        # Pillow is optional — if not installed, just skip dim probing.
        from PIL import Image
        with Image.open(local) as im:
            dims = f"{im.width}x{im.height}"
    except Exception:
        pass
    data = Path(local).read_bytes()
    b64 = base64.b64encode(data[:96 * 1024]).decode("ascii")
    return {"slot": slot, "remote": remote, "local": local,
            "dims": dims, "bytes": len(data),
            "preview_b64_first_96k": b64}


@app.tool("hal_logcat_tail",
          "Dump the last N lines of logcat, optionally regex-filtered. Default pattern matches cam_hook.",
          {"type": "object",
           "properties": {"lines":   {"type": "integer", "default": 200, "minimum": 10, "maximum": 5000},
                          "pattern": {"type": "string",  "default": "cam_hook"}},
           "required": []})
def _hal_logcat_tail(a):
    lines = int(a.get("lines", 200))
    pattern = a.get("pattern", "cam_hook")
    return logcat_dump(lines=lines, grep=pattern)   # str → text content block


@app.tool("hal_watch_captures",
          "Bounded logcat stream: collect structured cam_hook events (returnBuffer BLOB, write_blob RESCALE, write_yuv, snap, meta).",
          {"type": "object",
           "properties": {"duration_s": {"type": "number", "minimum": 1, "maximum": 300, "default": 15}},
           "required": []})
def _hal_watch_captures(a):
    dur = float(a.get("duration_s", 15))
    events = logcat_stream(dur, grep="cam_hook")
    return {"duration_s": dur, "count": len(events),
            "events": [ev.to_json() for ev in events]}


@app.tool("hal_probe_target",
          "Launch a KYC target package, watch camera mechanics for duration_s, return detected SDKs + negotiated stream dims + hook events + dim-match verdict. The 'make Wise work' tool.",
          {"type": "object",
           "properties": {"package":    {"type": "string",
                                         "description": "Android package, e.g. com.transferwise.android"},
                          "duration_s": {"type": "number", "default": 30, "minimum": 5, "maximum": 300},
                          "launch":     {"type": "boolean", "default": True},
                          "verbose":    {"type": "boolean", "default": False}},
           "required": ["package"]})
def _hal_probe_target(a):
    return probe.probe(a["package"], float(a.get("duration_s", 30)),
                       launch=bool(a.get("launch", True)),
                       verbose=bool(a.get("verbose", False)))


@app.tool("hal_adb_shell",
          "Escape hatch — run an arbitrary shell command on the device. On-device uses /system/bin/sh; over adb uses adb shell.",
          {"type": "object",
           "properties": {"cmd":     {"type": "string"},
                          "as_root": {"type": "boolean", "default": False}},
           "required": ["cmd"]})
def _hal_adb_shell(a):
    return adb_shell(a["cmd"], as_root=bool(a.get("as_root", False)), timeout=30.0)


# ── Resources ──────────────────────────────────────────────────────────────

@app.resource("hal://state", "hal state",
              "Daemon STATUS + every ctrl_* file on the device, one JSON blob.",
              "application/json")
def _res_state(uri): return json.dumps(daemon.status(), indent=2, default=str)


@app.resource("hal://stats", "hal stats",
              "Latest stats.json the daemon wrote.",
              "application/json")
def _res_stats(uri):
    return adb_shell(f"cat {CTRL_DIR}/stats.json 2>/dev/null || echo '{{}}'",
                     as_root=True, timeout=5)


@app.resource("hal://logcat", "recent cam_hook logcat",
              "Last 200 lines from logcat filtered to the cam_hook tag.",
              "text/plain")
def _res_logcat(uri): return logcat_dump(lines=200, grep="cam_hook")


# ── Entry ──────────────────────────────────────────────────────────────────

def serve_stdio() -> None:
    """Called from __main__ for the default stdio mode."""
    app.run_stdio()
