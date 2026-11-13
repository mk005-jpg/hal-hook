#!/system/bin/sh
# KernelSU install hook — runs once at `install from zip` time. Validates that
# AndroidHarness (which supplies the Python runtime) is present, and stages
# the launcher so AH can spawn `/data/adb/modules/hal_hook_mcp/bin/hal-hook-mcp`
# as a stdio MCP subprocess OR as an HTTP/SSE server on 127.0.0.1:7731.

ui_print "- hal-hook-mcp installer"

AH_LINUX=/data/user/0/com.androidharness.app/files/linux
AH_PY="${AH_LINUX}/bin/python3"

if [ ! -x "${AH_PY}" ]; then
    ui_print "! AndroidHarness python3 not found at ${AH_PY}"
    ui_print "! Install the main AndroidHarness app first, open it once so its"
    ui_print "! Linux chroot is unpacked, then flash this module again."
    abort "! missing AH chroot"
fi

ui_print "- AH python detected: $(${AH_PY} --version 2>&1 | head -1)"

# Make bin scripts executable
set_perm_recursive  $MODPATH               0 0 0755 0755
set_perm_recursive  $MODPATH/pylib         0 0 0755 0644
set_perm            $MODPATH/bin/hal-hook-mcp              0 0 0755
set_perm            $MODPATH/bin/hal-hook-mcp-install-deps 0 0 0755
set_perm            $MODPATH/service.sh                    0 0 0755

ui_print "- stdlib-only build: no pip deps to install"

ui_print "- done. In AndroidHarness, add MCP server:"
ui_print "    command: /data/adb/modules/hal_hook_mcp/bin/hal-hook-mcp"
ui_print "    (transport: stdio; no args needed)"
