"""Entry point: `python -m hal_hook_mcp` (stdio, default) or
`python -m hal_hook_mcp --http [--host H --port P --token T]` (SSE).

Stdio mode is for Claude Desktop / Claude Code which spawn the server as a
child process. HTTP/SSE mode is for AndroidHarness on-device (via `adb reverse
tcp:7731 tcp:7731`) or any other MCP client that speaks SSE.
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys


def _args():
    p = argparse.ArgumentParser(prog="hal-hook-mcp",
                                description="MCP server for hal-inject (stdio or HTTP/SSE)")
    p.add_argument("--http", action="store_true",
                   help="Serve MCP over HTTP/SSE instead of stdio.")
    p.add_argument("--host", default="127.0.0.1",
                   help="HTTP bind host (default: 127.0.0.1 — loopback only, "
                        "safe for `adb reverse` from the phone).")
    p.add_argument("--port", type=int, default=7731,
                   help="HTTP bind port (default: 7731).")
    p.add_argument("--token", default=os.environ.get("HAL_HOOK_MCP_TOKEN"),
                   help="Bearer token. If omitted, one is generated and printed. "
                        "Can also set $HAL_HOOK_MCP_TOKEN.")
    p.add_argument("--no-auth", action="store_true",
                   help="Disable bearer-token auth (DANGEROUS — only OK on a "
                        "strictly-loopback bind with no reverse forward).")
    return p.parse_args()


def main() -> None:
    level = os.environ.get("HAL_HOOK_MCP_LOG", "INFO").upper()
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-5s %(name)s: %(message)s",
        stream=sys.stderr,   # MCP stdio speaks on stdout — never contaminate it
    )
    args = _args()

    try:
        if args.http:
            from .http_server import serve_http
            token = None if args.no_auth else args.token  # None → generate
            serve_http(host=args.host, port=args.port, token=token)
        else:
            from .server import serve
            asyncio.run(serve())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
