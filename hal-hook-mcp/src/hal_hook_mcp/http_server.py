"""HTTP/SSE transport — makes the MCP reachable from AndroidHarness on-device
(via `adb reverse tcp:PORT tcp:PORT` and the fork's bearer-token pattern) and
from any other MCP client that speaks SSE instead of stdio.

Exposes:
  GET  /sse            — SSE event stream (server→client)
  POST /messages/...   — client→server JSON-RPC posts
  GET  /healthz        — unauthenticated liveness probe

Binds to 127.0.0.1 by default. If you expose on 0.0.0.0, PASS A TOKEN — the
hal daemon runs as root on the device and this MCP has full control over it;
an unauthenticated open port is an immediate RCE surface for anyone on the
local network. The `--token` flag enforces a `Authorization: Bearer <token>`
header on everything except /healthz.
"""
from __future__ import annotations

import logging
import secrets
from typing import Optional

from mcp.server.sse import SseServerTransport
from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import JSONResponse, Response
from starlette.routing import Mount, Route

from .server import app as mcp_app

log = logging.getLogger(__name__)


class BearerAuthASGI:
    """Pure-ASGI bearer-token wrapper. We can't use Starlette's
    `BaseHTTPMiddleware` because it buffers the full response, which breaks
    SSE streaming (AssertionError on http.response.start with no body).
    Direct ASGI gives us zero-copy pass-through for the SSE event stream
    while still gating the request before it reaches the app.

    Bearer tokens over plain HTTP are only safe on a trusted hop (adb reverse
    between the phone and your workstation, loopback, a SSH tunnel). On any
    exposed network put this behind TLS."""

    def __init__(self, app, token: str):
        self._app = app
        self._token = token
        self._header = f"Bearer {token}".encode("latin-1")

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            # websocket/lifespan pass through untouched
            return await self._app(scope, receive, send)
        # /healthz is always unauthenticated — lets a liveness probe work
        # without burning the token on it.
        path = scope.get("path", "")
        if path == "/healthz":
            return await self._app(scope, receive, send)
        headers = dict(scope.get("headers") or [])
        if headers.get(b"authorization") != self._header:
            await send({"type": "http.response.start", "status": 401,
                        "headers": [(b"content-type", b"application/json")]})
            await send({"type": "http.response.body",
                        "body": b'{"error":"unauthorized"}'})
            return
        return await self._app(scope, receive, send)


def _build_starlette(token: Optional[str]) -> Starlette:
    transport = SseServerTransport("/messages/")

    async def handle_sse(request: Request) -> Response:
        # Standard SSE-MCP handshake: hold a long-lived GET, feed server→client
        # JSON-RPC messages into the ReadableStream; the client posts replies to
        # the paired /messages/<session_id> endpoint which transport.handle_post_message
        # routes back to the Server instance.
        #
        # request._send is a private Starlette attribute but it's what the MCP
        # SseServerTransport docs use directly — no public alternative yet.
        async with transport.connect_sse(
            request.scope, request.receive, request._send
        ) as (read, write):
            await mcp_app.run(read, write, mcp_app.create_initialization_options())
        return Response()

    async def healthz(_: Request) -> JSONResponse:
        return JSONResponse({"ok": True, "transport": "sse"})

    starlette_app = Starlette(
        routes=[
            Route("/healthz", endpoint=healthz),
            Route("/sse", endpoint=handle_sse),
            Mount("/messages/", app=transport.handle_post_message),
        ],
    )
    if token:
        # Wrap outside Starlette so the bearer check runs BEFORE Starlette's
        # middleware stack touches the response — the only way SSE streams
        # don't trip BaseHTTPMiddleware's buffering assertion.
        return BearerAuthASGI(starlette_app, token=token)
    return starlette_app


def serve_http(host: str = "127.0.0.1", port: int = 7731,
               token: Optional[str] = None) -> None:
    """Blocking call. Prints the connection URL + token to stderr so the user
    can copy it into AndroidHarness / Claude Desktop."""
    # Lazy import so stdio mode has no dep on uvicorn.
    import uvicorn

    if token is None:
        token = secrets.token_urlsafe(24)
        print(f"[hal-hook-mcp] generated token: {token}", flush=True)

    url = f"http://{host}:{port}/sse"
    print(f"[hal-hook-mcp] serving MCP/SSE at {url}", flush=True)
    print(f"[hal-hook-mcp] client header: Authorization: Bearer {token}", flush=True)
    if host == "0.0.0.0":
        print("[hal-hook-mcp] WARNING: bound to 0.0.0.0 — token-only auth is "
              "exposed on the local network. Use TLS or bind 127.0.0.1 + "
              "`adb reverse` instead.", flush=True)

    starlette_app = _build_starlette(token)
    uvicorn.run(starlette_app, host=host, port=port, log_level="warning",
                access_log=False)
