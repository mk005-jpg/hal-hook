# hal-hook-mcp

MCP server that gives an AI agent full control over the **hal-inject** camera-injection framework on a rooted Pixel 7a — plus a KYC-target **probe plugin** that monitors any target app's camera mechanics and reports what the app negotiates vs what the hook delivers.

Works with any MCP client: Claude Desktop, Claude Code CLI, and on-device **AndroidHarness** (both as client-to-desktop-server via `adb forward`, and as a standalone on-device consumer if you run the MCP there).

## Capability

### Control
- `hal_status` — daemon STATUS + every `ctrl_*` file + cameraserver PID + libcam_hook map count
- `hal_start` / `hal_stop` — arm / disarm injection
- `hal_set_cam` — back / front / both
- `hal_set_slot` — idfront / idback
- `hal_set_fmt` — nv21 / nv12
- `hal_set_yuv_width` — min stream width to inject
- `hal_set_composite` — toggle Step-1 region compositor
- `hal_set_region` — paint rect in sensor-normalized 0..1 coords
- `hal_set_content_pad` — 0..200, content-PAD defense strength
- `hal_set_blob_res` — force BLOB rescale target or `(0, 0)` for AUTO
- `hal_push_slot_image` — host JPEG → device slot in one call
- `hal_snap_yuv` — dump pre-composite real scene + pull to host

### Live monitoring
- `hal_logcat_tail` — recent cam_hook lines (regex filter)
- `hal_watch_captures` — bounded logcat stream → structured events (returnBuffer BLOB dims, write_blob RESCALE targets, write_yuv activity, snaps, metadata)
- `hal_pull_slot_blob` — fetch the current frame_<slot>.jpg + dims + base64 preview (for sanity-checking what the hook is about to inject)

### Target-app probe (plugin)
- `hal_probe_target` — launch a KYC app, watch for `duration_s`, return:
  - Detected SDKs (Sumsub, FaceTec, iProov, Onfido, Jumio, Incode, Persona, Veriff, IDnow, Fourthline, ML Kit OCR)
  - Negotiated camera stream dims (preview YUV, still BLOB)
  - Hook-side events and the **dim-match verdict** between what the app expected and what the hook delivered — the "why is capture deformed?" killer

### Resources
- `hal://state` — current daemon + ctrl snapshot as JSON
- `hal://stats` — the daemon's stats.json
- `hal://logcat` — last 200 cam_hook lines

### Escape hatch
- `hal_adb_shell` — run any adb shell command on the device

## Transport

Host → device: `adb shell su -c 'echo CMD | nc -w 2 -U /data/local/tmp/hal_inject/daemon.sock'` per call (~60-120 ms). For streaming (logcat), `adb logcat` is piped through a `subprocess.Popen` with a hard timer.

Prereqs on the host:
- `adb` on PATH (or point `$ADB`)
- Optional: `$ADB_SERIAL` pins a specific device

Prereqs on the device:
- hal-inject daemon running (`system_health_check` or `hal_inject_daemon` process)
- Socket at `/data/local/tmp/hal_inject/daemon.sock`
- `su` available (Magisk / KernelSU)

## Install

```bash
cd hal-hook-mcp
pip install -e .
```

Or run without install:
```bash
python -m hal_hook_mcp
```

## Claude Desktop config

Add to `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS) or `%APPDATA%\Claude\claude_desktop_config.json` (Windows):

```json
{
  "mcpServers": {
    "hal-hook": {
      "command": "python",
      "args": ["-m", "hal_hook_mcp"],
      "env": {
        "ADB": "C:\\Users\\pc\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe",
        "HAL_HOOK_MCP_LOG": "INFO"
      }
    }
  }
}
```

## Claude Code config

Add to `~/.claude/mcp.json`:

```json
{
  "mcpServers": {
    "hal-hook": {
      "type": "stdio",
      "command": "python",
      "args": ["-m", "hal_hook_mcp"]
    }
  }
}
```

## AndroidHarness config (on-device)

AndroidHarness (Sanuu7 and forks) is an MCP *client*. To let it reach this server running on your desktop, expose MCP over a TCP forward:

1. Run the server as a stdio process behind a socat TCP wrapper on the desktop:
   ```bash
   socat TCP-LISTEN:7731,reuseaddr,fork EXEC:"python -m hal_hook_mcp"
   ```
2. On the phone, `adb reverse tcp:7731 tcp:7731` (adb must be running on the desktop too).
3. In AndroidHarness, add an MCP server at `tcp://127.0.0.1:7731` (or whatever transport your fork supports).

The `adel107107ai` fork documents a bearer-token + 127.0.0.1 pattern — if security matters, wrap `socat` with a TLS cert + bearer check.

## Typical session

```
> hal_status
{ok, ctrl_cam=back, ctrl_composite=1, ctrl_blob_res_w=0, ctrl_blob_res_h=0, ...}

> hal_probe_target  package=com.transferwise.android  duration_s=40
{
  sdk_detected: ["facetec", "iproov", "mlkit_ocr"],
  negotiated_streams: [{kind: "jpeg", w: 4032, h: 3024}],
  hook_blob_returns: [{w: 4032, h: 3024, should_inject: 1}],
  hook_rescales: [{tw: 4032, th: 3024, src: ".../frame_idfront.jpg"}],
  dim_match: { app_requested: "4032x3024", hook_delivered: "4032x3024", matches: true }
}
```

If `matches: false`, that's your "deformed" bug — either flip `hal_set_blob_res` to AUTO (`0, 0`) or force the app-expected dims.

## Files

```
hal-hook-mcp/
├── pyproject.toml
├── README.md
└── src/hal_hook_mcp/
    ├── __init__.py
    ├── __main__.py        # entry
    ├── server.py          # MCP registrations + dispatch
    ├── transport.py       # adb-nc daemon bridge
    ├── daemon.py          # typed command wrappers
    ├── monitor.py         # logcat dump + streaming
    ├── parsers.py         # cam_hook line parser → structured Event
    └── probe.py           # target-KYC-app probe plugin
```

## Known limits

- Per-call latency ~60-120 ms (new nc connection each command). For
  high-rate pushes, bundle multiple actions into one `hal_adb_shell` call.
- `hal_watch_captures` is blocking; agent cannot call other tools while it
  runs. Keep durations short.
- `probe` runs ONE logcat stream at a time (adb limits); SDK-marker grep is
  done on the recent logcat dump, not a live stream. So a very long-silent
  KYC flow can miss early SDK init events — launch the target yourself,
  then start the probe within 10 s.
