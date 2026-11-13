"""Entry point: `python -m hal_hook_mcp` (stdio-only, stdlib-only).

Spawned by MCP clients — Claude Desktop, Claude Code, AndroidHarness — as a
subprocess that speaks JSON-RPC over stdin/stdout. No pip deps, no network,
no HTTP server. Transport to the device auto-selects between native unix
socket (when running on-device) and adb shell (when running on PC)."""
from __future__ import annotations

import logging
import os
import sys


def main() -> None:
    level = os.environ.get("HAL_HOOK_MCP_LOG", "INFO").upper()
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-5s %(name)s: %(message)s",
        stream=sys.stderr,   # stdout is reserved for MCP JSON-RPC; never pollute
    )
    try:
        from .server import serve_stdio
        serve_stdio()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
