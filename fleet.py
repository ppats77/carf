"""Decode read-only diagnostic observations while retaining the adapter's reply."""
import re
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field

Vehicle = Literal["sienna_2009", "x3_2017_n55", "335i_2007_n54", "prius_2005"]
PAGES = {f"01{base:02X}" for base in range(0, 256, 32)}
REQUESTS = {
    "sienna_2009": {"0106", "0107", "0108", "0109", "21D9"},
    "x3_2017_n55": {"2257C3", "22DA12"},
    "335i_2007_n54": {"0123"},
    "prius_2005": {"21CE"},
}
# Only the addressed ECU may supply these measurements.
OEM_ECUS = {"21D9": 0x7E8, "2257C3": 0x612, "22DA12": 0x618, "21CE": 0x7EB}


class Observation(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    request: Annotated[str, Field(pattern=r"^(?:[0-9A-F]{4}|[0-9A-F]{6})$")]
    format: Literal["can11", "payload"]
    elapsed_ms: Annotated[int, Field(ge=0, le=2**63-1)]
    reply: Annotated[str, Field(max_length=447, pattern=r"^[ -~]*$")]


def payloads(obs):
    """Yield complete ISO-TP messages independently for each replying CAN ID."""
    partial = {}
    request = obs["request"]
    target = OEM_ECUS.get(request)
    for line in obs["reply"].split("|"):
        data = "".join(line.split())
        if not data or not re.fullmatch("[0-9A-Fa-f]+", data):
            continue
        if obs["format"] == "payload":
            if target is None and len(data) % 2 == 0:
                yield None, bytes.fromhex(data)
            continue
        if len(data) < 5 or len(data) > 19 or len(data) % 2 == 0:
            continue
        ecu, frame = int(data[:3], 16), bytes.fromhex(data[3:])
        if (target is not None and ecu != target) or (target is None and not 0x7E8 <= ecu <= 0x7EF):
            continue
        if request in ("2257C3", "22DA12"):
            if frame[:1] != b"\xf1":
                continue
            frame = frame[1:]
        if not frame:
            continue
        kind, nibble = divmod(frame[0], 16)
        if kind == 0:
            partial.pop(ecu, None)
            if 0 < nibble <= len(frame)-1:
                yield f"{ecu:03X}", frame[1:1+nibble]
        elif kind == 1:
            partial.pop(ecu, None)
            if len(frame) == 8:
                size = nibble * 256 + frame[1]
                if 7 < size <= 255:
                    partial[ecu] = (size, 1, bytearray(frame[2:]))
        elif kind == 2 and ecu in partial:
            size, sequence, body = partial.pop(ecu)
            if nibble != sequence or (len(frame) < 8 and len(body)+len(frame)-1 < size):
                continue
            body.extend(frame[1:])
            if len(body) >= size:
                yield f"{ecu:03X}", bytes(body[:size])
            else:
                partial[ecu] = (size, (sequence + 1) % 16, body)


def decode_observation(obs):
    """Return measurements or capability pages only for matching positive replies."""
    request = obs["request"]
    query = bytes.fromhex(request)
    prefix = bytes([query[0] + 0x40]) + query[1:]
    decoded = []
    for ecu, payload in payloads(obs):
        if not payload.startswith(prefix):
            continue
        raw = payload[len(prefix):]
        n = int.from_bytes(raw, "big")
        if request in PAGES and len(raw) == 4:
            base = query[1]
            values = {"page_base": f"{base:02X}",
                      "advertised_pids": [f"{base+i:02X}" for i in range(1, 32) if n & (1 << (32-i))],
                      "next_page_advertised": bool(n & 1)}
        elif request in ("0106", "0107", "0108", "0109") and len(raw) == 1:
            term = "short" if query[1] in (6, 8) else "long"
            bank = 1 if query[1] in (6, 7) else 2
            values = {f"{term}_term_fuel_trim_bank_{bank}_pct": (n - 128) * 100 / 128}
        elif request == "0123" and len(raw) == 2:
            values = {"fuel_rail_gauge_kpa": n * 10}
        elif request == "2257C3" and len(raw) == 2 and n <= 2000:
            values = {"engine_oil_c": n / 10}
        elif request == "22DA12" and len(raw) == 2 and n != 65535:
            values = {"transmission_fluid_c": n - 48}
        elif request == "21CE" and len(raw) >= 31 and raw[0] <= 200:
            blocks = [(int.from_bytes(raw[i:i+2], "big") - 32768) / 100 for i in range(3, 31, 2)]
            values = {"battery_soc_pct": raw[0] / 2,
                      "battery_current_a": (int.from_bytes(raw[1:3], "big") - 32768) / 100,
                      "battery_block_volts_1_to_14": blocks,
                      "battery_block_spread_v": round(max(blocks)-min(blocks), 2)}
        else:
            continue
        decoded.append({"ecu": ecu, **values})
    return decoded
