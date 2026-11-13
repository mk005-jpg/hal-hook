#!/system/bin/sh
# Runs once per boot (KernelSU late-start). Stdlib-only MCP has no pip deps —
# nothing to install. This file is kept as an extension point for future
# features (e.g. auto-starting an HTTP server at boot).
exit 0
