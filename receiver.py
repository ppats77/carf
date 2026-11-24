"""Receive bounded offline batches on the home LAN. A receipt means committed."""
import hashlib
import hmac
import os
from typing import Annotated
from uuid import UUID

import psycopg
from fastapi import FastAPI, HTTPException, Request
from pydantic import BaseModel, ConfigDict, Field, ValidationError
from psycopg.types.json import Jsonb
from fleet import Observation, PAGES, REQUESTS, Vehicle
from starlette.concurrency import run_in_threadpool
from starlette.responses import PlainTextResponse

app = FastAPI(openapi_url=None, docs_url=None, redoc_url=None)
TOKEN = os.environ["CARF_UPLOAD_TOKEN"]
MAX_BYTES = 65536
MAX_ROWS = 128


class Reading(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    trip_id: UUID
    elapsed_ms: Annotated[int, Field(ge=0, le=2**63 - 1)]
    vehicle: Vehicle | None = None
    observation: Observation | None = None
    utc_s: Annotated[int, Field(ge=0, le=253402300799)] | None = None
    rpm: Annotated[float, Field(ge=0, le=16383.75)] | None = None
    speed_kph: Annotated[int, Field(ge=0, le=255)] | None = None
    coolant_c: Annotated[int, Field(ge=-40, le=215)] | None = None


def parse(body: bytes):
    lines = body.splitlines()
    if not 1 <= len(lines) <= MAX_ROWS:
        raise HTTPException(422, "Expected 1 to 128 readings")
    rows = []
    vehicle = None
    for line in lines:
        try:
            point = Reading.model_validate_json(line)
        except ValidationError:
            raise HTTPException(422, "Invalid reading") from None
        if point.rpm is None and point.speed_kph is None and point.coolant_c is None:
            raise HTTPException(422, "Reading needs a measured value")
        if rows and (rows[-1][0] != point.trip_id or rows[-1][1] >= point.elapsed_ms):
            raise HTTPException(422, "Expected one trip in increasing elapsed time order")
        if rows and point.vehicle != vehicle:
            raise HTTPException(422, "Expected one vehicle profile per trip")
        vehicle = point.vehicle
        obs = point.observation
        if obs and (not vehicle or obs.request not in PAGES | REQUESTS[vehicle] or obs.elapsed_ms < point.elapsed_ms):
            raise HTTPException(422, "Observation does not match its vehicle or timestamp")
        rows.append((point.trip_id, point.elapsed_ms, point.utc_s, point.rpm, point.speed_kph, point.coolant_c,
                     obs.model_dump() if obs else None))
    return vehicle, rows


def commit(vehicle, rows):
    with psycopg.connect(connect_timeout=5) as conn:
        with conn.cursor() as cursor:
            # One insert and verification query per batch. A conflicting replay rolls back
            # the entire upload, including any new readings accompanying the conflict.
            trip_id = rows[0][0]
            cursor.execute("INSERT INTO public.trip_profiles VALUES (%s, %s) ON CONFLICT DO NOTHING",
                           (trip_id, vehicle))
            cursor.execute("SELECT vehicle FROM public.trip_profiles WHERE trip_id=%s", (trip_id,))
            if cursor.fetchone()[0] != vehicle:
                raise HTTPException(409, "Trip already belongs to another vehicle")
            cursor.executemany(
                "INSERT INTO public.readings (trip_id,elapsed_ms,utc_s,rpm,speed_kph,coolant_c,observation) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s) ON CONFLICT (trip_id,elapsed_ms) DO NOTHING",
                [(*row[:-1], Jsonb(row[-1]) if row[-1] is not None else None) for row in rows],
            )
            cursor.execute(
                "SELECT trip_id,elapsed_ms,utc_s,rpm,speed_kph,coolant_c,observation FROM public.readings "
                "WHERE trip_id=%s AND elapsed_ms=ANY(%s) ORDER BY elapsed_ms",
                (rows[0][0], [row[1] for row in rows]),
            )
            if cursor.fetchall() != rows:
                raise HTTPException(409, "Reading identity already has different values")


@app.get("/health")
def health():
    with psycopg.connect(connect_timeout=2) as conn:
        conn.execute("SELECT observation, vehicle FROM public.readings LEFT JOIN public.trip_profiles USING (trip_id) LIMIT 0")
    return {"ready": True}


@app.post("/v1/readings", response_class=PlainTextResponse)
async def ingest(request: Request):
    supplied = request.headers.get("authorization", "").encode()
    if not TOKEN or not hmac.compare_digest(supplied, ("Bearer " + TOKEN).encode()):
        raise HTTPException(401, "Invalid upload credentials")
    if request.headers.get("content-type", "").split(";")[0].strip() != "application/x-ndjson":
        raise HTTPException(415, "Expected application/x-ndjson")
    body = bytearray()
    async for part in request.stream():
        if len(body) + len(part) > MAX_BYTES:
            raise HTTPException(413, "Batch exceeds 65536 bytes")
        body.extend(part)
    vehicle, rows = parse(bytes(body))
    try:
        await run_in_threadpool(commit, vehicle, rows)
    except psycopg.OperationalError:
        raise HTTPException(503, "Database unavailable") from None
    return hashlib.sha256(body).hexdigest()
