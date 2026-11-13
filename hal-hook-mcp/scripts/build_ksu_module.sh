#!/usr/bin/env bash
# Build a flashable hal_hook_mcp KSU module zip.
# Output: dist/hal_hook_mcp-<version>.zip
# Usage:  scripts/build_ksu_module.sh

set -euo pipefail
cd "$(dirname "$0")/.."

ROOT=$(pwd)
OUT="${ROOT}/dist"
STAGE="${ROOT}/build/ksu-stage"
VER=$(grep '^version=' ksu-module/module.prop | cut -d= -f2)
ZIP="${OUT}/hal_hook_mcp-${VER}.zip"

rm -rf "${STAGE}" "${OUT}"
mkdir -p "${STAGE}/bin" "${STAGE}/pylib/hal_hook_mcp" "${OUT}"

# Module metadata + scripts
cp ksu-module/module.prop  "${STAGE}/"
cp ksu-module/customize.sh "${STAGE}/"
cp ksu-module/service.sh   "${STAGE}/"
cp ksu-module/bin/hal-hook-mcp              "${STAGE}/bin/"
cp ksu-module/bin/hal-hook-mcp-install-deps "${STAGE}/bin/"
chmod +x "${STAGE}/customize.sh" "${STAGE}/service.sh" \
         "${STAGE}/bin/hal-hook-mcp" "${STAGE}/bin/hal-hook-mcp-install-deps"

# The MCP package itself
cp -r src/hal_hook_mcp/* "${STAGE}/pylib/hal_hook_mcp/"
# Drop pycache + egg-info if present
find "${STAGE}/pylib" -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
find "${STAGE}/pylib" -type d -name '*.egg-info' -exec rm -rf {} + 2>/dev/null || true

# Package — zip with no top-level dir (KSU modules want flat root at zip root)
(cd "${STAGE}" && zip -qr "${ZIP}" .)

echo "built: ${ZIP}"
ls -lah "${ZIP}"
