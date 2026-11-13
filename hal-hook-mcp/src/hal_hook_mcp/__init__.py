"""hal-hook-mcp: MCP server for the hal-inject camera-injection framework.

Wraps the on-device unix-domain socket daemon (/data/local/tmp/hal_inject/
daemon.sock) in an MCP interface so Claude / AndroidHarness / any other MCP
client can drive the hook, watch live captures, and probe target KYC apps.

Transport: `adb shell su -c 'echo CMD | nc -U <sock>'` for v1 — zero device-
side changes. Upgrade to a tcp-forward + persistent connection later if the
per-call round-trip becomes a bottleneck.
"""
__version__ = "0.1.0"
