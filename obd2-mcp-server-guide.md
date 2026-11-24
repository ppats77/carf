# OBD-II → MCP Server (Python, read-only)

Connect a car's OBD-II port to an LLM client (Claude Code, Claude Desktop, or a local MCP host) through a small Python MCP server built on python-OBD and FastMCP.

This is a corrected version of an earlier draft. What changed is listed under "Review notes" at the end.

---

## Architecture

```
[Vehicle OBD-II port]
        │  CAN / ISO 15765-4 (or older K-line protocols)
        ▼
[OBD-II adapter]       classic Bluetooth SPP, USB, or BLE + ble-serial bridge
        │  serial port (/dev/tty.*, /dev/rfcomm*, COMx)
        ▼
[Python MCP server]    FastMCP + python-OBD (obd 0.7.3)
        │  stdio / JSON-RPC
        ▼
[LLM client]           Claude Code, Claude Desktop, local host
```

---

## 1. Hardware

python-OBD can only reach an adapter through a **serial port** (it uses pyserial). Choose an adapter based on that:

| Adapter | Link | Works with python-OBD directly? |
|---|---|---|
| OBDLink MX+ | Bluetooth 3.0 (classic SPP) | Yes. Pairs as a serial port |
| Vgate vLinker FD **BT3.0** | Classic Bluetooth | Yes. Pairs as a serial port |
| OBDLink EX / vLinker FS USB | USB | Yes. USB serial device |
| OBDLink CX | BLE 5.1 only (custom BLE serial service) | No. Needs a BLE→serial bridge |
| Vgate vLinker FD**+** BLE4.0 | BLE | No. Needs a BLE→serial bridge |

Note: "vLinker FD" and "vLinker FD+" are separate products. Check which one you're buying.

**Using a BLE adapter anyway:** run [ble-serial](https://github.com/Jakeler/ble-serial) (`pip install ble-serial`). It creates a virtual serial port (for example `/tmp/ttyBLE` on macOS/Linux, or a COM port via com0com on Windows), and you point python-OBD at that port.

Avoid cheap unbranded ELM327 "v2.1" clones. They often drop frames or fail initialization.

---

## 2. Safety: keep it read-only

Never give the LLM raw AT/ST pass-through or arbitrary CAN/UDS frame transmission.

**Exposed (read-only):**
- Mode 01: live data
- Mode 02: freeze-frame data
- Mode 03: stored DTCs
- Mode 07: pending DTCs
- Mode 09: vehicle info (VIN)

**Not exposed:**
- Mode 04 (clear DTCs / reset monitors). If you ever want it, do it outside the LLM with a manual confirmation step.
- Raw `AT`/`ST` commands, UDS writes (`0x2E`, `0x31`, `0x34`…), and any security access.

---

## 3. Setup

```bash
mkdir obd2-mcp-server && cd obd2-mcp-server
python3 -m venv .venv
source .venv/bin/activate
pip install mcp obd
# only if using a BLE adapter:
# pip install ble-serial
```

Find your serial port:
- macOS: `ls /dev/tty.*` after pairing (e.g. `/dev/tty.OBDLinkMX...`)
- Linux: `/dev/rfcomm0` (after `rfcomm bind`) or `/dev/ttyUSB0`
- BLE via ble-serial: the port it prints (default `/tmp/ttyBLE`)

Set it in the environment so the server doesn't auto-scan every port:

```bash
export OBD_PORT=/dev/tty.OBDLinkMX-STN-SPP   # example
```

---

## 4. `server.py`

```python
"""Read-only OBD-II MCP server.

Important: this server speaks MCP over stdio. Never print() to stdout;
it corrupts the protocol. python-OBD logs to stderr, which is fine.
"""

import os
import sys
import logging

import obd
from mcp.server.fastmcp import FastMCP

logging.basicConfig(stream=sys.stderr, level=logging.WARNING)
obd.logger.setLevel(logging.WARNING)

mcp = FastMCP("obd2-diagnostics")

PORT = os.environ.get("OBD_PORT")  # None → python-OBD auto-scan (slow)
_connection: obd.OBD | None = None


def get_connection() -> obd.OBD:
    global _connection
    if _connection is None or not _connection.is_connected():
        if _connection is not None:
            _connection.close()
        _connection = obd.OBD(portstr=PORT, fast=False, timeout=5)
    return _connection


def _q(conn: obd.OBD, cmd) -> str | None:
    """Query a command if supported; return a string value or None."""
    if not conn.supports(cmd):
        return None
    res = conn.query(cmd)
    return None if res.is_null() else str(res.value)


def _dtcs(conn: obd.OBD, cmd) -> list[dict]:
    res = conn.query(cmd)
    if res.is_null() or not res.value:
        return []
    return [{"code": code, "description": desc} for code, desc in res.value]


@mcp.tool()
def get_vehicle_status() -> dict:
    """Check OBD-II adapter connection status, port and protocol."""
    conn = get_connection()
    return {
        "connected": conn.is_connected(),
        "status": str(conn.status()),
        "port": conn.port_name(),
        "protocol": conn.protocol_name(),
    }


@mcp.tool()
def read_dtc() -> dict:
    """Read stored (Mode 03) and pending (Mode 07) Diagnostic Trouble Codes."""
    conn = get_connection()
    if not conn.is_connected():
        return {"error": "OBD-II adapter not connected."}
    return {
        "stored_dtcs": _dtcs(conn, obd.commands.GET_DTC),
        "pending_dtcs": _dtcs(conn, obd.commands.GET_CURRENT_DTC),
    }


LIVE_PIDS = [
    obd.commands.RPM,
    obd.commands.SPEED,
    obd.commands.COOLANT_TEMP,
    obd.commands.INTAKE_TEMP,
    obd.commands.ENGINE_LOAD,
    obd.commands.THROTTLE_POS,
    obd.commands.SHORT_FUEL_TRIM_1,
    obd.commands.LONG_FUEL_TRIM_1,
    obd.commands.CONTROL_MODULE_VOLTAGE,
]


@mcp.tool()
def get_live_data() -> dict:
    """Fetch key real-time engine and sensor telemetry (Mode 01)."""
    conn = get_connection()
    if not conn.is_connected():
        return {"error": "OBD-II adapter not connected."}
    readings = {}
    for cmd in LIVE_PIDS:
        val = _q(conn, cmd)
        if val is not None:
            readings[cmd.name] = val
    return {"telemetry": readings}


@mcp.tool()
def get_freeze_frame() -> dict:
    """Read freeze-frame data captured when a fault was stored (Mode 02).

    Uses python-OBD's DTC_* commands (Mode 02), NOT live Mode 01 values.
    ECUs vary; missing fields come back as None.
    """
    conn = get_connection()
    if not conn.is_connected():
        return {"error": "OBD-II adapter not connected."}

    freeze = conn.query(obd.commands.FREEZE_DTC)  # 0102: DTC that triggered the frame
    snapshot = {}
    for name in ("DTC_RPM", "DTC_ENGINE_LOAD", "DTC_COOLANT_TEMP",
                 "DTC_SPEED", "DTC_SHORT_FUEL_TRIM_1", "DTC_LONG_FUEL_TRIM_1"):
        cmd = obd.commands[name]
        try:
            res = conn.query(cmd, force=True)  # mode 02 may not be in the supported-PID map
            snapshot[name] = None if res.is_null() else str(res.value)
        except Exception as e:  # noqa: BLE001
            snapshot[name] = f"error: {e}"

    return {
        "freeze_dtc": None if freeze.is_null() else str(freeze.value),
        "snapshot": snapshot,
    }


@mcp.tool()
def get_vehicle_info() -> dict:
    """Read vehicle information (Mode 09): VIN and calibration ID, if supported."""
    conn = get_connection()
    if not conn.is_connected():
        return {"error": "OBD-II adapter not connected."}
    return {
        "vin": _q(conn, obd.commands.VIN),
        "calibration_id": _q(conn, obd.commands.CALIBRATION_ID),
    }


if __name__ == "__main__":
    mcp.run()
```

---

## 5. Connecting a client

**Claude Code:**

```bash
claude mcp add obd2-diagnostics \
  --env OBD_PORT=/dev/tty.OBDLinkMX-STN-SPP \
  -- /absolute/path/to/obd2-mcp-server/.venv/bin/python /absolute/path/to/obd2-mcp-server/server.py
```

**Claude Desktop / generic MCP host** (`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "obd2-diagnostics": {
      "command": "/absolute/path/to/obd2-mcp-server/.venv/bin/python",
      "args": ["/absolute/path/to/obd2-mcp-server/server.py"],
      "env": { "OBD_PORT": "/dev/tty.OBDLinkMX-STN-SPP" }
    }
  }
}
```

### Example prompts
- "Check my car for stored or pending trouble codes and explain likely causes."
- "Pull live fuel trims, coolant temp and module voltage. Is anything out of spec?"
- "Compare the freeze-frame data with the stored code to narrow down the root cause."

---

## 6. Test checklist (for Claude Code)

- [ ] Before testing on the car, verify that the `DTC_*` command names exist in the installed `obd` version: `python -c "import obd; print([c.name for c in obd.commands[2]][:10])"`
- [ ] Use `python-OBD`'s connection to confirm the port opens with the ignition on (`get_vehicle_status`)
- [ ] Confirm that `force=True` Mode 02 queries return data on the target ECU; some return NO DATA unless a DTC is stored
- [ ] Make sure nothing writes to stdout (run the server and check that only JSON-RPC appears)
- [ ] Optional: add a test with a mocked `obd.OBD` so the tools can be exercised without a car

---

## Review notes (what changed from the original draft)

1. **Freeze frame was wrong.** The original `get_freeze_frame` queried live Mode 01 PIDs (`RPM`, `ENGINE_LOAD`, `COOLANT_TEMP`) and labelled them as a snapshot. The fix uses python-OBD's Mode 02 `DTC_*` commands.
2. **BLE adapters don't work directly.** The OBDLink CX is BLE-only and vLinker FD+ is BLE, so neither appears as a serial port. The original listed `bleak` but never used it. The fix is to use a classic-Bluetooth or USB adapter, or bridge with ble-serial.
3. **Explicit port.** `OBD_PORT` replaces auto-scan, which can make the first tool call hang.
4. **Support checks** are now consistent across tools.
5. **Mode 09 added** (`get_vehicle_info`). The safety section allowed it, but no tool implemented it.
6. **DTC output** is returned as `{code, description}` objects instead of raw tuples.
7. **stdout hygiene.** Logging goes to stderr, with a comment warning against `print()`.
