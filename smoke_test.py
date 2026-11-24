"""Rehearse an upload -> PostgreSQL -> MCP round trip in a disposable stack."""

import asyncio
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
from uuid import uuid4

from fleet import decode_observation

import psycopg
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


ENDPOINT = "http://receiver:8787/v1/readings"


def body(rows):
    return b"".join((json.dumps(row) + "\n").encode() for row in rows)


def upload(payload, token):
    request = Request(
        ENDPOINT,
        data=payload,
        headers={
            "Authorization": "Bearer " + token,
            "Content-Type": "application/x-ndjson",
        },
    )
    try:
        with urlopen(request, timeout=8) as response:
            return response.status, response.read()
    except HTTPError as error:
        return error.code, error.read()


def rehearse_upload(trip_id):
    # The points are synthetic. Missing or unknown values should stay null.
    records = [
        {"trip_id": trip_id, "elapsed_ms": 1000, "utc_s": None,
         "rpm": 800.25, "speed_kph": 0, "coolant_c": 35},
        {"trip_id": trip_id, "elapsed_ms": 2000, "utc_s": 1760000002,
         "rpm": None, "speed_kph": 0, "coolant_c": 36},
        {"trip_id": trip_id, "elapsed_ms": 7000, "utc_s": 1760000007,
         "rpm": 1600, "speed_kph": 20, "coolant_c": None},
    ]
    payload = body(records)
    # Wait for the HTTP listener while PostgreSQL initializes on a fresh volume.
    for attempt in range(30):
        try:
            code, _ = upload(payload, "invalid-rehearsal-token")
            if code in (401, 403):
                break
            if code != 503:
                raise AssertionError(f"Unauthorized upload returned HTTP {code}")
        except URLError:
            pass
        time.sleep(1)
    else:
        raise RuntimeError("Receiver did not become ready for the rehearsal.")

    token = os.environ["CARF_UPLOAD_TOKEN"]
    expected = (200, hashlib.sha256(payload).hexdigest().encode("ascii"))
    with ThreadPoolExecutor(max_workers=2) as pool:
        replies = list(pool.map(lambda _: upload(payload, token), range(2)))
    assert replies == [expected, expected], f"Unexpected upload receipts: {replies!r}"
    # A new point and a conflicting duplicate share a batch. Neither may commit.
    collision = body([
        dict(records[0], elapsed_ms=0),
        dict(records[0], rpm=999),
    ])
    assert upload(collision, token)[0] == 409, "Conflicting replay must be rejected."


async def read_tool(session, name, arguments):
    result = await session.call_tool(name, arguments)
    if result.isError:
        raise AssertionError(f"{name} failed: {result.content}")
    if result.structuredContent is not None:
        return result.structuredContent
    return json.loads(result.content[0].text)


async def rehearse_mcp(trip_id):
    reader_env = {k: v for k, v in os.environ.items() if k in
                  {"PGHOST", "PGPORT", "PGDATABASE", "PGUSER", "PGPASSWORD", "PGSSLMODE"}}
    server = StdioServerParameters(
        command=sys.executable,
        args=[str(Path(__file__).with_name("mcp_server.py"))],
        env=reader_env,
    )
    async with stdio_client(server) as (receive, send):
        async with ClientSession(receive, send) as client:
            await client.initialize()
            tools = (await client.list_tools()).tools
            assert {tool.name for tool in tools} == {
                "list_trips", "summarize_trip", "read_trip_samples"
            }
            assert all(tool.annotations.readOnlyHint for tool in tools)
            trips = await read_tool(client, "list_trips", {"limit": 5})
            assert trips["trips"][0]["trip_id"] == trip_id
            args = {"trip_id": trip_id}
            summary = await read_tool(client, "summarize_trip", args)
            assert summary["sample_count"] == 3, summary
            assert summary["observed_span_ms"] == 6000
            assert summary["largest_sample_gap_ms"] == 5000
            assert summary["samples_with_clock"] == 2
            rpm = summary["metrics"]["rpm"]
            assert rpm["min"] == 800.25 and rpm["max"] == 1600 and rpm["missing"] == 1
            assert summary["metrics"]["coolant_c"]["sample_mean"] == 35.5
            page = await read_tool(client, "read_trip_samples", {**args, "limit": 2})
            assert [r["elapsed_ms"] for r in page["samples"]] == [1000, 2000]
            assert page["samples"][0]["utc_s"] is None and page["samples"][1]["rpm"] is None
            last = await read_tool(client, "read_trip_samples", {
                **args, "start_ms": page["next_start_ms"], "end_ms": 7000,
            })
            assert [r["elapsed_ms"] for r in last["samples"]] == [7000]
            assert last["next_start_ms"] is None
            assert (await client.call_tool("read_trip_samples", {**args, "limit": 201})).isError
            print(f"MCP read {trip_id}: 3 synthetic samples, peak 1600 RPM, 1 missing RPM.")


def check_fleet():
    token = os.environ["CARF_UPLOAD_TOKEN"]
    battery = bytes.fromhex("61CE788000") + b"".join((34368+i).to_bytes(2, "big") for i in range(14))
    frames = [f"7EB10{len(battery):02X}" + battery[:6].hex()]
    for offset in range(6, len(battery), 7):
        frames.append(f"7EB2{1+(offset-6)//7:X}" + battery[offset:offset+7].hex())
    cases = [
        ("sienna_2009", "0106", "7E803410680", "short_term_fuel_trim_bank_1_pct", 0),
        ("335i_2007_n54", "0123", "7E804412301F4", "fuel_rail_gauge_kpa", 5000),
        ("x3_2017_n55", "2257C3", "612F1056257C304AD", "engine_oil_c", 119.7),
        ("x3_2017_n55", "22DA12", "618F10562DA12007F", "transmission_fluid_c", 79),
        ("prius_2005", "21CE", "|".join(frames), "battery_block_spread_v", 0.13),
    ]
    async def verify():
        server = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name("mcp_server.py"))],
                                       env={k: v for k, v in os.environ.items() if k.startswith("PG")})
        async with stdio_client(server) as streams, ClientSession(*streams) as client:
            await client.initialize()
            for vehicle, request, reply, key, expected in cases:
                trip = str(uuid4())
                obs = {"request": request, "elapsed_ms": 2, "format": "can11", "reply": reply}
                row = {"trip_id": trip, "vehicle": vehicle, "elapsed_ms": 0, "rpm": 0, "observation": obs}
                payload = body([row])
                assert upload(payload, token) == (200, hashlib.sha256(payload).hexdigest().encode())
                assert upload(payload, token)[0] == 200
                assert upload(body([{**row, "observation": {**obs, "reply": "NO DATA"}}]), token)[0] == 409
                matches = await read_tool(client, "list_trips", {"vehicle": vehicle})
                assert matches["trips"][0]["trip_id"] == trip
                samples = await read_tool(client, "read_trip_samples", {"trip_id": trip, "request": request})
                saved = samples["samples"][0]["observation"]
                assert saved["reply"] == reply and saved["decoded"][0][key] == expected, saved
                other = await read_tool(client, "read_trip_samples", {"trip_id": trip, "request": "0100"})
                assert other["samples"] == []
                assert upload(body([{"trip_id":trip, "elapsed_ms":3, "speed_kph":0}]), token)[0] == 409
                if request == "21CE":
                    assert saved["decoded"][0]["battery_block_volts_1_to_14"] == [16+i/100 for i in range(14)]
                    assert saved["decoded"][0]["battery_soc_pct"] == 60
            for reply in ("NO DATA", "7EB 03 7F 21 12", "|".join(frames[1:]),
                          "|".join(frames[:1]+frames[2:]), "|".join(frames[:-1])):
                assert not decode_observation({"format":"can11", "request":"21CE", "reply":reply})
            assert not decode_observation({"format":"can11", "request":"22DA12", "reply":"612F10562DA12007F"})
            pages = decode_observation({"format":"can11", "request":"0100",
                                        "reply":"7E8 06 41 00 00 10 00 01|7E9 06 41 00 00 08 00 00"})
            assert [r["advertised_pids"] for r in pages] == [["0C"], ["0D"]]
            assert pages[0]["next_page_advertised"] and not pages[1]["next_page_advertised"]
    asyncio.run(verify())
    print("All four vehicle profiles passed receiver, replay and MCP checks.")


def check_reader_role():
    with psycopg.connect("") as db:
        rights = db.execute("""
            SELECT current_user,
                   has_table_privilege('public.readings', 'SELECT'),
                   has_table_privilege('public.readings', 'INSERT,UPDATE,DELETE,TRUNCATE')
        """).fetchone()
    assert rights == ("carf_reader", True, False), f"Unexpected reader permissions: {rights}"


if __name__ == "__main__":
    trip = str(uuid4())
    check_reader_role()
    check_fleet()
    rehearse_upload(trip)
    asyncio.run(rehearse_mcp(trip))
    print("Passed authorization, commit receipt, concurrent replay, rollback, reader permissions, and MCP pagination.")
