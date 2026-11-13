"""MCP server registrations.

Exposes hal-inject daemon control, logcat streaming, and the KYC-target probe
plugin as MCP tools + resources. Transport: stdio (default) — the client
(Claude Desktop, AndroidHarness, etc.) spawns `hal-hook-mcp` and talks over
stdin/stdout. HTTP/SSE can be added later; see README.

Tools return JSON-serializable dicts; MCP's CallToolResult wraps them."""
from __future__ import annotations

import base64
import json
import logging
import os
import tempfile
from pathlib import Path
from typing import Optional

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import (
    Resource,
    TextContent,
    Tool,
)

from . import daemon, monitor, probe
from .transport import adb_pull, adb_push, adb_shell, check_daemon_up, CTRL_DIR

log = logging.getLogger(__name__)

app = Server("hal-hook-mcp")

# ── Tool catalogue ─────────────────────────────────────────────────────────

TOOLS: list[Tool] = [
    Tool(
        name="hal_status",
        description="Snapshot of the hal-inject daemon state: STATUS JSON, every ctrl_* file, cameraserver PID + libcam_hook map count. Call this first in any session to confirm the stack is up.",
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    Tool(
        name="hal_start",
        description="Arm the daemon (equivalent of the Android app's Start button). Enables injection.",
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    Tool(
        name="hal_stop",
        description="Disarm the daemon. Camera traffic passes through unmodified.",
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    Tool(
        name="hal_set_cam",
        description="Pick which camera facing to inject on: 'back' (ID doc capture), 'front' (selfie) or 'both'.",
        inputSchema={"type": "object",
                     "properties": {"mode": {"type": "string",
                                              "enum": ["back", "front", "both"]}},
                     "required": ["mode"]},
    ),
    Tool(
        name="hal_set_slot",
        description="Activate a prepared media slot ('idfront' or 'idback'). Prepare slots via hal_push_slot_image.",
        inputSchema={"type": "object",
                     "properties": {"slot": {"type": "string",
                                              "enum": ["idfront", "idback"]}},
                     "required": ["slot"]},
    ),
    Tool(
        name="hal_set_fmt",
        description="YUV chroma order for the injected slot: 'nv21' or 'nv12'.",
        inputSchema={"type": "object",
                     "properties": {"fmt": {"type": "string",
                                             "enum": ["nv21", "nv12"]}},
                     "required": ["fmt"]},
    ),
    Tool(
        name="hal_set_yuv_width",
        description="Minimum YUV stream width to inject on (0 disables YUV inject). BLOB inject is unaffected.",
        inputSchema={"type": "object",
                     "properties": {"width": {"type": "integer", "minimum": 0}},
                     "required": ["width"]},
    ),
    Tool(
        name="hal_set_composite",
        description="Toggle the Step-1 region compositor (1=region composite+realism passes, 0=legacy full-overwrite).",
        inputSchema={"type": "object",
                     "properties": {"on": {"type": "boolean"}},
                     "required": ["on"]},
    ),
    Tool(
        name="hal_set_region",
        description="Where in the sensor frame the ID is painted. All floats normalized 0..1.",
        inputSchema={"type": "object",
                     "properties": {"cx": {"type": "number"}, "cy": {"type": "number"},
                                     "w": {"type": "number"}, "h": {"type": "number"}},
                     "required": ["cx", "cy", "w", "h"]},
    ),
    Tool(
        name="hal_set_content_pad",
        description="Content-PAD strength 0..200 (fibre + laminate-specular pass vs 'digital screen' detectors).",
        inputSchema={"type": "object",
                     "properties": {"percent": {"type": "integer", "minimum": 0, "maximum": 200}},
                     "required": ["percent"]},
    ),
    Tool(
        name="hal_set_blob_res",
        description="Force BLOB rescale target (w, h). (0, 0) = AUTO: hook matches KYC-negotiated stream dims per call.",
        inputSchema={"type": "object",
                     "properties": {"w": {"type": "integer", "minimum": 0},
                                     "h": {"type": "integer", "minimum": 0}},
                     "required": ["w", "h"]},
    ),
    Tool(
        name="hal_push_slot_image",
        description="Push a YUV and/or BLOB JPEG from the host into a slot. local_yuv and/or local_blob are host-side absolute paths. The daemon converts them to frame_<slot>.{nv21,nv12,jpg} and applies.",
        inputSchema={"type": "object",
                     "properties": {"slot": {"type": "string",
                                              "enum": ["idfront", "idback"]},
                                     "local_yuv": {"type": "string"},
                                     "local_blob": {"type": "string"}},
                     "required": ["slot"]},
    ),
    Tool(
        name="hal_snap_yuv",
        description="Dump the next pre-composite real YUV frame (or the cached one if no live stream) to <device_dst>, then pull to the host. Returns WxH and the saved local path.",
        inputSchema={"type": "object",
                     "properties": {"local_dst": {"type": "string"}},
                     "required": ["local_dst"]},
    ),
    Tool(
        name="hal_pull_slot_blob",
        description="Fetch the current frame_<slot>.jpg from the device as base64 + its dims. Useful to verify what the hook is about to inject.",
        inputSchema={"type": "object",
                     "properties": {"slot": {"type": "string",
                                              "enum": ["idfront", "idback"]}},
                     "required": ["slot"]},
    ),
    Tool(
        name="hal_logcat_tail",
        description="Dump the last N lines of logcat, optionally regex-filtered. Default pattern matches cam_hook.",
        inputSchema={"type": "object",
                     "properties": {"lines": {"type": "integer", "default": 200, "minimum": 10, "maximum": 5000},
                                     "pattern": {"type": "string", "default": "cam_hook"}},
                     "required": []},
    ),
    Tool(
        name="hal_watch_captures",
        description="Open a bounded logcat stream and collect structured cam_hook events (returnBuffer BLOB, write_blob RESCALE, write_yuv, snap, meta). Use duration_s 5-60.",
        inputSchema={"type": "object",
                     "properties": {"duration_s": {"type": "number", "minimum": 1, "maximum": 300, "default": 15}},
                     "required": []},
    ),
    Tool(
        name="hal_probe_target",
        description="Launch (or attach to) a KYC target package, watch its camera mechanics for duration_s, and return a structured report: detected SDKs, negotiated stream dims, hook injection events, dim-match analysis. The 'make Wise work' first-call tool.",
        inputSchema={"type": "object",
                     "properties": {"package": {"type": "string",
                                                 "description": "Android package, e.g. com.transferwise.android"},
                                     "duration_s": {"type": "number", "default": 30, "minimum": 5, "maximum": 300},
                                     "launch": {"type": "boolean", "default": True,
                                                "description": "If true, monkey-launch the target before watching."},
                                     "verbose": {"type": "boolean", "default": False}},
                     "required": ["package"]},
    ),
    Tool(
        name="hal_adb_shell",
        description="Escape hatch — run an arbitrary adb shell command on the device (as_root optional). For anything the typed tools don't cover.",
        inputSchema={"type": "object",
                     "properties": {"cmd": {"type": "string"},
                                     "as_root": {"type": "boolean", "default": False}},
                     "required": ["cmd"]},
    ),
]


@app.list_tools()
async def list_tools() -> list[Tool]:
    return TOOLS


# ── Tool dispatch ──────────────────────────────────────────────────────────

def _t(obj) -> list[TextContent]:
    return [TextContent(type="text", text=json.dumps(obj, indent=2, default=str))]


@app.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    try:
        if name == "hal_status":
            up, msg = check_daemon_up()
            if not up:
                return _t({"ok": False, "error": msg})
            return _t(daemon.status())

        if name == "hal_start":           return _t(daemon.start())
        if name == "hal_stop":            return _t(daemon.stop())
        if name == "hal_set_cam":         return _t(daemon.set_cam(arguments["mode"]))
        if name == "hal_set_slot":        return _t(daemon.set_slot(arguments["slot"]))
        if name == "hal_set_fmt":         return _t(daemon.set_fmt(arguments["fmt"]))
        if name == "hal_set_yuv_width":   return _t(daemon.set_yuv_width(arguments["width"]))
        if name == "hal_set_composite":   return _t(daemon.set_composite(arguments["on"]))
        if name == "hal_set_region":
            return _t(daemon.set_region(arguments["cx"], arguments["cy"],
                                        arguments["w"], arguments["h"]))
        if name == "hal_set_content_pad": return _t(daemon.set_content_pad(arguments["percent"]))
        if name == "hal_set_blob_res":    return _t(daemon.set_blob_res(arguments["w"], arguments["h"]))

        if name == "hal_push_slot_image":
            slot = arguments["slot"]
            ly = arguments.get("local_yuv")
            lb = arguments.get("local_blob")
            if not ly and not lb:
                return _t({"ok": False, "error": "need at least one of local_yuv, local_blob"})
            # Push to device cache then call SET_SLOT_IMAGE. The daemon copies
            # them into its own CTRL_DIR with the right names.
            dev_cache = f"/data/local/tmp/hal_inject_push_{slot}"
            adb_shell(f"mkdir -p {dev_cache}", as_root=True, timeout=5)
            dev_yuv = f"{dev_cache}/yuv.jpg" if ly else ""
            dev_blob = f"{dev_cache}/blob.jpg" if lb else ""
            if ly: adb_push(ly, dev_yuv)
            if lb: adb_push(lb, dev_blob)
            # chmod so the daemon (root) can read them from either path
            adb_shell(f"chmod -R 644 {dev_cache}/*.jpg 2>/dev/null || true",
                      as_root=True, timeout=5)
            r = daemon.set_slot_image(slot, dev_yuv or dev_blob,
                                       dev_blob or dev_yuv)
            return _t({**r, "pushed": {"yuv": dev_yuv, "blob": dev_blob}})

        if name == "hal_snap_yuv":
            local_dst = arguments["local_dst"]
            # ask daemon to put it in a known device path (world-readable by root)
            device_dst = "/data/local/tmp/hal_inject_mcp_snap.nv21"
            r = daemon.snap_yuv(device_dst)
            if not r.get("ok"):
                return _t(r)
            # chmod + pull
            adb_shell(f"chmod 644 {device_dst} {device_dst.rsplit('.',1)[0]}.dim",
                      as_root=True, timeout=5)
            os.makedirs(os.path.dirname(os.path.abspath(local_dst)) or ".", exist_ok=True)
            adb_pull(device_dst, local_dst)
            adb_pull(device_dst.rsplit(".", 1)[0] + ".dim", local_dst + ".dim")
            return _t({**r, "local_path": local_dst})

        if name == "hal_pull_slot_blob":
            slot = arguments["slot"]
            remote = f"{CTRL_DIR}/frame_{slot}.jpg"
            with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as tf:
                local = tf.name
            # chmod in case the mode drifted
            adb_shell(f"chmod 644 {remote} 2>/dev/null || true",
                      as_root=True, timeout=5)
            adb_pull(remote, local)
            dims = None
            try:
                from PIL import Image
                with Image.open(local) as im:
                    dims = f"{im.width}x{im.height}"
            except Exception:
                pass
            data = Path(local).read_bytes()
            b64 = base64.b64encode(data[:96 * 1024]).decode("ascii")  # cap preview at 96 KiB
            return _t({"slot": slot, "remote": remote, "local": local,
                       "dims": dims, "bytes": len(data),
                       "preview_b64_first_96k": b64})

        if name == "hal_logcat_tail":
            lines = int(arguments.get("lines", 200))
            pattern = arguments.get("pattern", "cam_hook")
            text = monitor.logcat_dump(lines=lines, grep=pattern)
            return [TextContent(type="text", text=text)]

        if name == "hal_watch_captures":
            dur = float(arguments.get("duration_s", 15))
            events = monitor.logcat_stream(dur, grep="cam_hook")
            return _t({"duration_s": dur, "count": len(events),
                       "events": [ev.to_json() for ev in events]})

        if name == "hal_probe_target":
            pkg = arguments["package"]
            dur = float(arguments.get("duration_s", 30))
            launch = bool(arguments.get("launch", True))
            verbose = bool(arguments.get("verbose", False))
            return _t(probe.probe(pkg, dur, launch=launch, verbose=verbose))

        if name == "hal_adb_shell":
            cmd = arguments["cmd"]
            as_root = bool(arguments.get("as_root", False))
            out = adb_shell(cmd, as_root=as_root, timeout=30.0)
            return [TextContent(type="text", text=out)]

        return _t({"ok": False, "error": f"unknown tool: {name}"})

    except Exception as e:
        log.exception("tool %s failed", name)
        return _t({"ok": False, "error": str(e), "type": type(e).__name__})


# ── Resources ──────────────────────────────────────────────────────────────

RESOURCES: list[Resource] = [
    Resource(uri="hal://state",  name="hal state",
             description="Daemon STATUS + every ctrl_* file on the device, one JSON blob.",
             mimeType="application/json"),
    Resource(uri="hal://stats",  name="hal stats",
             description="Latest stats.json the daemon wrote (bump counters, last frame info).",
             mimeType="application/json"),
    Resource(uri="hal://logcat",  name="recent cam_hook logcat",
             description="Last 200 lines from logcat filtered to the cam_hook tag.",
             mimeType="text/plain"),
]


@app.list_resources()
async def list_resources() -> list[Resource]:
    return RESOURCES


@app.read_resource()
async def read_resource(uri: str) -> str:
    if uri == "hal://state":
        return json.dumps(daemon.status(), indent=2, default=str)
    if uri == "hal://stats":
        out = adb_shell(f"cat {CTRL_DIR}/stats.json 2>/dev/null || echo '{{}}'",
                        as_root=True, timeout=5)
        return out
    if uri == "hal://logcat":
        return monitor.logcat_dump(lines=200, grep="cam_hook")
    raise ValueError(f"unknown resource: {uri}")


# ── Entry ──────────────────────────────────────────────────────────────────

async def serve() -> None:
    """Stdio server loop. Called from __main__."""
    async with stdio_server() as (read, write):
        await app.run(read, write, app.create_initialization_options())
