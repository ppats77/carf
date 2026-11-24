"""Read-only trip analysis for MCP clients. Start with python mcp_server.py."""

from typing import Annotated
from uuid import UUID

import psycopg
from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from psycopg.rows import dict_row
from pydantic import Field
from fleet import decode_observation, Vehicle


mcp = FastMCP("CARF trip logs")
read_only = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
Milliseconds = Annotated[int, Field(ge=0, le=9223372036854775807)]


def reader():
    return psycopg.connect(
        "",
        row_factory=dict_row,
        connect_timeout=5,
        options="-c default_transaction_read_only=on -c statement_timeout=15000",
    )


@mcp.tool(annotations=read_only)
def list_trips(
    limit: Annotated[int, Field(ge=1, le=100)] = 20,
    offset: Annotated[int, Field(ge=0)] = 0,
    vehicle: Vehicle | None = None,
) -> dict:
    """List trips in latest upload order. UTC fields are recorded Unix seconds or null.
    last_received_at is the NAS upload time and can differ from when the car was driven.
    Use next_offset for further pages. New uploads can change the ordering.
    """
    with reader() as db:
        rows = db.execute(
            """
            SELECT trip_id::text, vehicle, count(*) AS sample_count,
                   min(elapsed_ms) AS first_elapsed_ms, max(elapsed_ms) AS last_elapsed_ms,
                   min(utc_s) AS earliest_known_utc_s, max(utc_s) AS latest_known_utc_s,
                   max(received_at) AS last_received_at
            FROM public.readings
            LEFT JOIN public.trip_profiles USING (trip_id)
            WHERE (%s::text IS NULL OR vehicle = %s)
            GROUP BY trip_id, vehicle
            ORDER BY max(received_at) DESC, trip_id
            LIMIT %s OFFSET %s
            """,
            (vehicle, vehicle, limit + 1, offset),
        ).fetchall()
    trips = rows[:limit]
    for trip in trips:
        trip["last_received_at"] = trip["last_received_at"].isoformat()
    return {"trips": trips, "next_offset": offset + limit if len(rows) > limit else None}


@mcp.tool(annotations=read_only)
def summarize_trip(trip_id: UUID) -> dict:
    """Summarize observed RPM, speed (km/h), and coolant (°C) from one trip.
    Means are over measured samples. Counts and largest sample gap describe coverage.
    Use read_trip_samples for vehicle-specific signals and discovery replies.
    observed_span_ms runs from first to last sample and can include recording gaps.
    """
    with reader() as db:
        summary = db.execute(
            """
            SELECT count(*) AS sample_count,
                   min(elapsed_ms) AS first_elapsed_ms, max(elapsed_ms) AS last_elapsed_ms,
                   max(elapsed_ms) - min(elapsed_ms) AS observed_span_ms,
                   max(gap_ms) AS largest_sample_gap_ms,
                   count(utc_s) AS samples_with_clock,
                   count(rpm) AS rpm_count, min(rpm) AS rpm_min, max(rpm) AS rpm_max,
                   avg(rpm)::float8 AS rpm_mean,
                   count(speed_kph) AS speed_kph_count, min(speed_kph) AS speed_kph_min,
                   max(speed_kph) AS speed_kph_max, avg(speed_kph)::float8 AS speed_kph_mean,
                   count(coolant_c) AS coolant_c_count, min(coolant_c) AS coolant_c_min,
                   max(coolant_c) AS coolant_c_max, avg(coolant_c)::float8 AS coolant_c_mean
            FROM (
                SELECT *, elapsed_ms - lag(elapsed_ms) OVER (ORDER BY elapsed_ms) AS gap_ms
                FROM public.readings WHERE trip_id = %s
            ) AS trip_samples
            """,
            (trip_id,),
        ).fetchone()
    if summary["sample_count"] == 0:
        raise ValueError("No readings found for that trip ID.")
    metrics = {}
    for name, unit in (("rpm", "rev/min"), ("speed_kph", "km/h"), ("coolant_c", "°C")):
        count = summary.pop(f"{name}_count")
        metrics[name] = {
            "unit": unit,
            "measured": count,
            "missing": summary["sample_count"] - count,
            "min": summary.pop(f"{name}_min"),
            "max": summary.pop(f"{name}_max"),
            "sample_mean": summary.pop(f"{name}_mean"),
        }
    return {"trip_id": str(trip_id), **summary, "metrics": metrics}


@mcp.tool(annotations=read_only)
def read_trip_samples(
    trip_id: UUID,
    start_ms: Milliseconds = 0,
    end_ms: Milliseconds | None = None,
    limit: Annotated[int, Field(ge=1, le=200)] = 50,
    request: Annotated[str, Field(pattern=r"^(?:[0-9A-F]{4}|[0-9A-F]{6})$")] | None = None,
) -> dict:
    """Inspect samples including diagnostic observations, raw replies and decoded vehicle signals.
    Optionally filter by hexadecimal request, e.g. 21CE for Prius battery samples.
    Battery block voltages are ordered 1 through 14. Empty decoded means no usable positive reply.
    Observations carry their own completion time in elapsed_ms. Both window bounds apply to the base sample.
    Keep end_ms and pass next_start_ms as start_ms to continue. Null means unavailable.
    utc_s is Unix time; rpm is rev/min, speed_kph is km/h, coolant_c is °C.
    """
    if end_ms is not None and end_ms < start_ms:
        raise ValueError("end_ms must be greater than or equal to start_ms.")
    with reader() as db:
        rows = db.execute(
            """
            SELECT elapsed_ms, utc_s, rpm, speed_kph, coolant_c, observation
            FROM public.readings
            WHERE trip_id = %s AND elapsed_ms >= %s
              AND elapsed_ms <= coalesce(%s::bigint, 9223372036854775807)
            AND (%s::text IS NULL OR observation->>'request' = %s)
            ORDER BY elapsed_ms LIMIT %s
            """,
            (trip_id, start_ms, end_ms, request, request, limit + 1),
        ).fetchall()
    for row in rows[:limit]:
        obs = row.get("observation")
        if obs:
            obs["decoded"] = decode_observation(obs)
    return {
        "trip_id": str(trip_id),
        "samples": rows[:limit],
        "next_start_ms": rows[limit]["elapsed_ms"] if len(rows) > limit else None,
    }


if __name__ == "__main__":
    mcp.run(transport="stdio")
