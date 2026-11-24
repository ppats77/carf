"""Record a bounded, receive-only classic CAN session on the MX+ Bluetooth serial port."""
import argparse
import json
import re
import time

from mx_serial import connection_args, setting


def setup(port):
    # Raw ISO 11898, 500 kbit/s. Both CAN ID widths reach the raw pass filter.
    for command in ("ATD", "ATE0", "ATL0", "STP 31", "STCMM 0", "ATH1", "ATS0",
                    "ATD0", "STFAC", "STFPA 00,00", "ATCF 000", "ATCM 000"):
        setting(port, command)


def frame(line):
    compact = "".join(line.split()).upper()
    if not re.fullmatch(r"[A-F0-9]+", compact):
        return {"adapter_status": line}
    width = 3 if len(compact) % 2 else 8
    identifier, data = compact[:width], compact[width:]
    if len(identifier) != width or len(data) > 16 or int(identifier, 16) > (0x7ff if width == 3 else 0x1fffffff):
        return {"adapter_status": line}
    return {"can_id": identifier, "data_hex": data, "length": len(data)//2}


def record(port, emit, seconds):
    start = time.monotonic()
    pending = bytearray()
    port.write(b"STM\r")
    try:
        while time.monotonic() - start < seconds:
            chunk = port.read(port.in_waiting or 1)
            for byte in chunk:
                if byte in (10, 13, 62):
                    if pending:
                        line = pending.decode("ascii", errors="replace")
                        emit({"elapsed_ms": round((time.monotonic()-start)*1000), **frame(line)})
                        pending.clear()
                    if byte == 62:
                        return "adapter stopped"
                elif len(pending) < 256:
                    pending.append(byte)
                else:
                    raise RuntimeError("Overlong adapter line")
        return "duration reached"
    finally:
        port.write(b" ")  # Interrupt STM. A bare carriage return could repeat an already finished command.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    connection_args(parser)
    parser.add_argument("--seconds", type=int, default=15, choices=range(1, 121), metavar="1..120")
    args = parser.parse_args()
    import serial  # pyserial, also included in the PlatformIO Python environment
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as out, serial.Serial(args.port, 115200, timeout=0.1) as port:
        def emit(row):
            out.write(json.dumps(row, separators=(",", ":")) + "\n")
        setup(port)
        port.timeout = 0.1
        emit({"vehicle": args.vehicle, "started_unix_s": time.time(), "bitrate": 500000,
              "mode": "silent", "timestamps": "host receive time"})
        reason = record(port, emit, args.seconds)
        emit({"end": reason})
    print(f"Saved {args.output} ({reason})")


if __name__ == "__main__":
    main()
