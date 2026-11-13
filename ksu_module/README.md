# HAL Inject SEPolicy Module

Grants the two SELinux allowances the injection chain needs on Android 13/14,
so the in-process `libcam_hook.so` path works end to end.

## What it does

Applied from `sepolicy.rule` natively by KernelSU / KernelSU-Next at boot
(with `service.sh` as a live fallback for Magisk or older setups):

1. **ptrace** — lets the injector's root domain `PTRACE_ATTACH` `cameraserver`
   (domain `cameraserver`, *not* `hal_camera_default` — that is the HAL, not the
   service the hook is loaded into):
   ```
   allow ksu    cameraserver process ptrace
   allow su     cameraserver process ptrace
   allow shell  cameraserver process ptrace
   ```
2. **file access** — `libcam_hook.so` runs *inside* `cameraserver` and reads its
   inputs from `/data/local/tmp/hal_inject` (label `shell_data_file`): the hook
   `.so` (for `dlopen`), `ctrl` / `ctrl_yuv` / `frame.jpg` / `frame.nv21` /
   `frame.dim`, and it writes `stats.json`. `cameraserver` cannot touch
   `shell_data_file` by default, so the module grants the dir traversal + file
   read/map + stats write it needs.

3. **execmem/execmod** — the in-process inline hook needs an executable
   trampoline (`execmem`) and to patch cameraserver's own `.text`
   (`execmod` on `cameraserver_exec`).
4. **app socket** — lets the companion app (`untrusted_app`) `connectto` the
   daemon's control socket under `/data/local/tmp/hal_inject` (served by the
   `ksu` root domain), so the GUI's buttons drive the daemon:
   ```
   allow untrusted_app shell_data_file dir search
   allow untrusted_app shell_data_file sock_file write
   allow untrusted_app ksu unix_stream_socket connectto
   ```

Without (2), `PTRACE_ATTACH` succeeds but `dlopen` returns NULL — cameraserver
can't even `search` `/data/local/tmp`. That was the real blocker, not ptrace.
Without (4) the app shows "Daemon: NOT CONNECTED" (hardware Vol± toggles still
work, as they are handled entirely by the root daemon).

## Where the hook .so must live, and why

The module ships `libcam_hook.so` at **`system/lib64/`**, so it is mounted to
`/system/lib64/libcam_hook.so` (label `system_lib_file`). This is required:

On the Pixel 7a (lynx, A14) the `[system]` section of
`/linkerconfig/ld.config.txt` — which governs `/system/bin/cameraserver` — sets
`namespace.default.isolated = true` with `search.paths = /system/${LIB}`. In an
isolated namespace a `dlopen` from `/data/local/tmp` does **not** load, even
though `/data` appears in `permitted.paths` (the symptom is a bare
`"...libcam_hook.so" is not a directory (ignoring)` warning and a NULL return,
with no SELinux denial). A `.so` under `/system/lib64` is on the namespace
search path and loads cleanly. The daemon prefers `/system/lib64/libcam_hook.so`
and falls back to the `/data` copy only for ad-hoc testing.

Two more things make the remote load work, both in `cam_injector`:
- It plants a `BRK #0` breakpoint at the return address so completion is
  detected deterministically (returning to arbitrary bytes could run instead of
  faulting, hanging the tracer while cameraserver stays ptrace-stopped).
- It anchors the dlopen **caller address** inside a *default-namespace* library
  (`libcameraservice.so`) — **not** `libc.so`, which lives in the runtime APEX
  namespace whose permitted paths are APEX-only.

The runtime control/frame files still live in `/data/local/tmp/hal_inject`
(`shell_data_file`), so rule (2) above is still needed for the hook to read them.

## Install

```
# zip the folder contents (module.prop at the zip root), then:
#   KernelSU Manager → Modules → Install from storage → reboot
```

`sepolicy.rule` is applied natively by KernelSU at boot. `service.sh` re-applies
the same rules live via `magiskpolicy` / `sepolicy-inject` / `ksud sepolicy`,
whichever exists — KernelSU-Next ships `ksud`, not `magiskpolicy`, so the old
magiskpolicy-only script was a silent no-op there.

## YUV injection (optional)

By default only BLOB (still-capture) streams are replaced — safe for document
scan flows. To also inject back-camera video (e.g. a synthetic stream), set a
minimum back-cam width via the app's "YUV video" toggle, or:

```
echo 3840 > /data/local/tmp/hal_inject/ctrl_yuv   # 4K only — leaves 1080p alone
```

Front-camera liveness is never touched (back camera only).
