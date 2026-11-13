#!/system/bin/sh
# hal_inject — apply the SELinux rules live at each boot.
#
# This is a belt-and-suspenders companion to sepolicy.rule (which KernelSU /
# KernelSU-Next applies natively, and which is the primary mechanism). It also
# covers Magisk, and KernelSU builds where the native rule load is unavailable.
#
# Tool preference: magiskpolicy (Magisk) → sepolicy-inject → ksud sepolicy
# (KernelSU-Next ships ksud, NOT magiskpolicy — the old magiskpolicy-only
# version of this script was a no-op there).

RULES="
allow ksu cameraserver process ptrace
allow su cameraserver process ptrace
allow shell cameraserver process ptrace
allow cameraserver shell_data_file dir { search open read getattr add_name write }
allow cameraserver shell_data_file file { open read getattr map execute execute_no_trans create write }
allow cameraserver cameraserver process execmem
allow cameraserver cameraserver_exec file execmod
allow untrusted_app shell_data_file dir search
allow untrusted_app shell_data_file sock_file write
allow untrusted_app ksu unix_stream_socket connectto
allow ksu app_data_file dir search
allow ksu app_data_file file { read open getattr }
"

apply() {   # $1 = one sepolicy statement
    if command -v magiskpolicy >/dev/null 2>&1; then
        magiskpolicy --live "$1"
    elif command -v sepolicy-inject >/dev/null 2>&1; then
        sepolicy-inject --live "$1"
    elif command -v ksud >/dev/null 2>&1; then
        ksud sepolicy patch "$1"
    else
        return 1
    fi
}

echo "$RULES" | while IFS= read -r r; do
    [ -z "$r" ] && continue
    if ! apply "$r"; then
        echo "[hal_inject] no live sepolicy tool — relying on sepolicy.rule" >&2
        break
    fi
done

echo "[hal_inject] sepolicy rules applied" >&2

# Start the injection daemon in the background once the system has settled. It only
# listens (ctrl stays off until the app/user sends START, which ptrace-loads the
# hook), and its single-instance guard makes a duplicate launch a harmless no-op.
DAEMON=/data/local/tmp/hal_inject/hal_inject_daemon
if [ -x "$DAEMON" ]; then
    ( sleep 25
      rm -f /data/local/tmp/hal_inject/daemon.sock
      setsid "$DAEMON" >/data/local/tmp/hal_inject/daemon.log 2>&1 </dev/null &
    ) &
    echo "[hal_inject] daemon scheduled" >&2
fi
