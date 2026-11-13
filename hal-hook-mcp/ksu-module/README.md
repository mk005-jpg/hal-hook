# hal_hook_mcp — on-device KSU module

Flashable KernelSU module that installs the hal-hook-mcp server on the phone
so AndroidHarness can drive hal-inject with **no PC and no adb**.

## Install

```bash
# On a desktop (one-time build):
scripts/build_ksu_module.sh
# produces dist/hal_hook_mcp-v0.1.0.zip

# Copy to device + flash via KernelSU manager (Modules → Install from storage).
# Reboot.
```

Or install directly over adb for development:

```bash
adb push dist/hal_hook_mcp-v0.1.0.zip /sdcard/
adb shell su -c "ksud module install /sdcard/hal_hook_mcp-v0.1.0.zip"
adb reboot
```

**Prerequisite**: AndroidHarness must be installed AND opened at least once
so its Linux chroot (`/data/user/0/com.androidharness.app/files/linux/`) is
unpacked. The module uses AH's bundled `python3.14` as its runtime — it does
NOT ship its own interpreter.

## What happens on first boot

1. `service.sh` waits up to 60s for AH's chroot to appear
2. `bin/hal-hook-mcp-install-deps` runs AH's pip to install `mcp`, `pillow`,
   `starlette`, `uvicorn` into `/data/adb/modules/hal_hook_mcp/deps/` (a
   module-private site-packages — doesn't touch AH's env)
3. Marker file `.deps-pending` is removed
4. Log goes to `/data/adb/modules/hal_hook_mcp/install.log`

Flashing takes ~2s. First-boot deps install takes ~20s. After that boots are
noop.

## Wire up AndroidHarness

In AH's MCP server settings add ONE server:

### Stdio (preferred — no network, no auth)

```
command:     /data/adb/modules/hal_hook_mcp/bin/hal-hook-mcp
args:        []
```

### HTTP/SSE (if AH's stdio support is limited in your fork)

Start it once (e.g. via AH's shell tool, or a KSU post-fs-data script):

```bash
/data/adb/modules/hal_hook_mcp/bin/hal-hook-mcp --http --port 7731 --token mysecret
```

Then in AH:

```
url:      http://127.0.0.1:7731/sse
headers:  Authorization: Bearer mysecret
```

## Layout

```
/data/adb/modules/hal_hook_mcp/
├── module.prop
├── customize.sh                 # install-time validation
├── service.sh                   # late-start: first-boot deps install
├── bin/
│   ├── hal-hook-mcp             # launcher — sets LD_LIBRARY_PATH + execs python
│   └── hal-hook-mcp-install-deps
├── pylib/
│   └── hal_hook_mcp/            # the actual package
├── deps/                        # created at first boot by pip
└── install.log
```

## Transport auto-detect

The MCP's `transport.py` picks the backend at import time by checking
whether `/data/local/tmp/hal_inject/daemon.sock` exists as a real local
file. On-device → `native` (direct AF_UNIX socket + `open()` on ctrl files +
`/system/bin/logcat` subprocess). On a PC → `adb` fallback (same code, same
tools, just a different transport).

Force via env: `HAL_HOOK_TRANSPORT=native` or `=adb`.

## Updating the pylib after a code change

Edit `src/hal_hook_mcp/*.py` on your desktop, rebuild the zip, reflash — OR
push the updated files directly during development:

```bash
adb push src/hal_hook_mcp/transport_native.py \
    /data/adb/modules/hal_hook_mcp/pylib/hal_hook_mcp/
```

Changes take effect on the next MCP spawn (next AH session / next
`hal-hook-mcp` invocation). No reboot.
