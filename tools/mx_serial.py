"""Small synchronous MX+ serial transport for parked diagnostic sessions."""
from pathlib import Path
import time

VEHICLES = ("sienna_2009", "x3_2017_n55", "335i_2007_n54", "prius_2005")


def exchange(port, command, seconds=5):
    """Wait for the prompt before allowing another request on this connection."""
    port.write(command.encode("ascii") + b"\r")
    deadline = time.monotonic() + seconds
    reply = bytearray()
    while time.monotonic() < deadline:
        reply.extend(port.read(port.in_waiting or 1))
        if reply.endswith(b">"):
            return reply[:-1].decode("ascii", errors="replace").strip()
        if len(reply) > 32768:
            break
    raise RuntimeError(f"Incomplete MX+ response to {command}: {bytes(reply[:160])!r}")


def setting(port, command):
    if "OK" not in exchange(port, command).split():
        raise RuntimeError(f"Adapter rejected {command}")


def connection_args(parser):
    parser.add_argument("--port", required=True)
    parser.add_argument("--vehicle", required=True, choices=VEHICLES)
    parser.add_argument("--output", type=Path, required=True)
