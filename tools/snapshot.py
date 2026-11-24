"""Survey advertised live OBD-II PIDs and read an emissions diagnostic snapshot."""
import argparse
import json
import re
import time

from mx_serial import connection_args, exchange, setting


def bitmap_rows(reply, protocol, base):
    """Validate response service, PID and length before scheduling further reads."""
    for line in reply.splitlines():
        compact = "".join(line.split()).upper()
        ecu = None
        if protocol in ("6", "8"):
            m = re.fullmatch(r"([0-7][0-9A-F]{2})06(41[0-9A-F]{10})(?:[0-9A-F]{2})?", compact)
        elif protocol in ("7", "9"):
            m = re.fullmatch(r"([0-1][0-9A-F]{7})06(41[0-9A-F]{10})(?:[0-9A-F]{2})?", compact)
        else:
            m = None
            if not re.fullmatch(r"41[0-9A-F]{10}", compact):
                continue
        if protocol in ("6", "7", "8", "9"):
            if not m:
                continue
            ecu, compact = m.groups()
        if int(compact[2:4], 16) == base:
            yield ecu, int(compact[4:], 16)


def survey(port, emit):
    for cmd in ("ATD", "ATE0", "ATL0", "ATH0", "ATS1", "ATSP0"):
        setting(port, cmd)
    first = exchange(port, "0100", 20)
    if not list(bitmap_rows(first, "", 0)):
        emit({"request": "0100", "reply": first, "status": "no valid discovery reply"})
        return
    protocol = exchange(port, "ATDPN").removeprefix("A")
    emit({"protocol": protocol})
    if protocol in ("6", "7", "8", "9"):
        setting(port, "ATH1")
        setting(port, "ATD0")
    def read(command):
        reply = exchange(port, command)
        emit({"received_unix_s": time.time(), "request": command, "reply": reply})
        return reply
    supported = set()
    for base in range(0, 256, 32):
        reply = read(f"01{base:02X}")
        more = False
        for ecu, mask in bitmap_rows(reply, protocol, base):
            pids = [base + n for n in range(1, 32) if mask & (1 << (32-n))]
            emit({"responder": ecu, "page_base": f"{base:02X}",
                  "advertised_pids": [f"{p:02X}" for p in pids], "has_next_page": bool(mask & 1)})
            supported.update(pids)
            more |= bool(mask & 1)
        if not more:
            break
    for pid in sorted(supported):
        read(f"01{pid:02X}")
    # Stored, pending and permanent emissions DTCs, freeze-frame trigger, monitor and vehicle-info support.
    for command in ("03", "07", "0A", "020200", "0600", "0900"):
        read(command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    connection_args(parser)
    args = parser.parse_args()
    import serial
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as out, serial.Serial(args.port, 115200, timeout=0.1) as port:
        def emit(row):
            out.write(json.dumps(row, separators=(",", ":")) + "\n")
            out.flush()
        emit({"vehicle": args.vehicle, "started_unix_s": time.time(), "mode": "parked_survey"})
        survey(port, emit)
        emit({"end_unix_s": time.time()})
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
