"""Entry point: `python -m hal_hook_mcp` or the `hal-hook-mcp` script."""
from __future__ import annotations

import asyncio
import logging
import os
import sys


def main() -> None:
    level = os.environ.get("HAL_HOOK_MCP_LOG", "INFO").upper()
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-5s %(name)s: %(message)s",
        stream=sys.stderr,   # MCP speaks on stdout — never contaminate it
    )
    from .server import serve
    try:
        asyncio.run(serve())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
