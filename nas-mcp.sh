#!/bin/sh
# Called via ssh -T. Keep stdout exclusive to the MCP protocol.
set -eu
PATH="/usr/local/bin:/var/packages/ContainerManager/target/usr/bin:$PATH"
export PATH
cd "$(dirname "$0")"
if docker compose version >/dev/null 2>&1; then
  exec docker compose -f compose.yaml -p carf run --rm --no-deps -T mcp
fi
exec docker-compose -f compose.yaml -p carf run --rm --no-deps -T mcp
