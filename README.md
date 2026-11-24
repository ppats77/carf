# CARF

CARF records trips on an M5Stack Core2 v1.3 paired with an OBDLink MX+. It saves readings to microSD, uploads them on home Wi-Fi, and lets an LLM inspect the saved trips through MCP. Core2 records standard readings and has selectable profiles for the 2009 Sienna, 2017 X3 xDrive35i, 2007 335i convertible and 2005 Prius. See the [vehicle signals and CAN collection guide](VEHICLES.md).

```text
Car → MX+ → Bluetooth Classic → Core2 / microSD
                                    │
                                home Wi-Fi
                                    ↓
                           receiver → PostgreSQL ← read-only MCP ← Claude
                                         Synology
```

## Rehearse on the Mac

Run from this directory with Docker and Compose available. The rehearsal creates its own project, generates temporary credentials, uploads synthetic vehicle-profile observations and a three-sample baseline trip, queries them over MCP, and removes its containers and volume. It checks simultaneous retries, conflicting data, missing measurements, and database permissions.

```sh
python3 rehearse.py
```

With Claude Code signed in, this version also asks Claude to analyze the three synthetic samples. It should find a minimum speed of 0 km/h, one missing RPM value, and a peak of 1,600 RPM at 7,000 ms with coolant unavailable at that moment. On a separate simulated Prius recording it should identify block 1 as the lowest and report a 0.13 V spread.

```sh
python3 rehearse.py --claude
```

## Start PostgreSQL on Synology

Use a NAS that supports Container Manager, with a 64-bit Intel/AMD or ARM processor. The examples use the project name `carf` and the directory `/volume1/docker/carf`. Substitute your actual volume and SSH alias. Extract [carf-synology.tar.gz](dist/carf-synology.tar.gz) into that directory. Alternatively, copy these source files with DSM File Station, keeping `db/init.sql` inside `db`.

```text
compose.yaml   Dockerfile        requirements.txt   .dockerignore
db/init.sql    setup_env.py      receiver.py        mcp_server.py
nas-mcp.sh     fleet.py          db/fleet.sql
```

In that directory on the NAS, generate credentials once. Keep the resulting private `.env` alongside `compose.yaml`. PostgreSQL reads its initialization settings when creating a fresh database volume. Keep the credentials and volume together across restarts.

```sh
python3 setup_env.py
docker compose -p carf up -d --build receiver
docker compose -p carf ps
curl --fail http://127.0.0.1:8787/health
```

If DSM provides standalone Compose, use `docker-compose -f compose.yaml`. In Container Manager's Project interface, use the same project name and directory. The health endpoint should return `{"ready":true}`. From the home Wi-Fi, repeat the request using the NAS LAN address. The firmware uses port 8787 and HTTP bearer authentication over the home network. Restrict that port to the trusted LAN in DSM. PostgreSQL stays on the private Compose network. Its data persists in the `carf_pgdata` volume when containers restart. [Synology Container Manager projects](https://kb.synology.com/en-us/DSM/help/ContainerManager/docker_project?version=7)

### Upgrade an existing CARF installation

Copy the updated package to the existing project directory, preserving the private `.env` and database volume. Apply the additive migration as the database owner before rebuilding the receiver. Run from the NAS project directory. On older DSM systems prefix Docker commands with the existing authorized sudo configuration and use standalone Compose if needed.

```sh
docker compose -f compose.yaml -p carf exec -T db psql -U postgres -d carf < db/fleet.sql
docker compose -f compose.yaml -p carf up -d --build receiver
```

## Connect Claude to the saved trips

The Mac launches the NAS MCP server over SSH. Use an SSH alias that already supports key authentication and can run Docker on the NAS. The alias below is an example. The launcher starts a short-lived MCP process with the PostgreSQL reader role. Each tool response goes to the LLM client you chose.

```sh
claude mcp add --transport stdio carf -- ssh -T -o BatchMode=yes YOUR_NAS_ALIAS sh /volume1/docker/carf/nas-mcp.sh
claude mcp get carf
```

Open Claude Code in the same project and ask:

> List my recorded trips. For the most recently uploaded trip, give me peak RPM, the observed speed range, and the number of missing coolant readings. Cite the trip ID and units.

A fresh installation should return an empty trip list. The three available tools are `list_trips`, `summarize_trip`, and `read_trip_samples`. Ask for a time window to zoom in, or ask Claude to compare two trip IDs. Trip ordering uses the upload time. Elapsed milliseconds describe time within a recording, and UTC timestamps depend on the logger's clock. A reconnect can create a second recording during the same drive. The existing `obd2-mcp-server-guide.md` describes an earlier live-adapter design.

## First test with the car

1. On the Mac, run `python3 configure_logger.py`. Enter the home SSID, Wi-Fi password, NAS LAN IP (a DHCP reservation keeps it stable), and the `CARF_UPLOAD_TOKEN` value from the NAS's private `.env`. Only the upload token is needed from that file. The helper stores the settings in a private header excluded from version control.
2. Insert a FAT32 microSD card, connect the Core2 using the USB-C data cable, and flash it following the [firmware instructions](firmware/README.md).
3. With the car parked, pair the adapter, let the sample count increase for a minute, and confirm Claude can retrieve the logged values.
4. Repeat with the receiver temporarily stopped. Start it again and confirm the buffered readings arrive with the original trip ID and elapsed times.

The Core2 removes a sealed batch from the card after the receiver commits it and returns the matching SHA-256 digest. A lost response can safely trigger another upload of the same measurements.

## What Claude can tell you about a trip

The Core2 asks the vehicle which standard OBD-II parameters it supports (`01 00`). It then polls the supported parameters below in sequence. The examples show synthetic replies from separate requests. The Core2 decodes them before saving a sample to microSD.

| Measurement | Request | Example reply | Value Claude receives |
| --- | --- | --- | --- |
| Engine speed | `01 0C` | `41 0C 1A F8` | `rpm: 1726` (0x1AF8 divided by 4) |
| Vehicle speed | `01 0D` | `41 0D 00` | `speed_kph: 0` (km/h) |
| Engine coolant temperature | `01 05` | `41 05 78` | `coolant_c: 80` (0x78 minus 40, in °C) |

Each saved row has a recording ID (`trip_id`) and an elapsed time (`elapsed_ms`) measured from the first saved sampling cycle. The timestamp marks the end of the poll cycle. The logger also saves Unix time in seconds (`utc_s`) when its clock is set. It waits one second between cycles, so use the recorded timestamps to measure intervals.

Claude uses `list_trips` to select a recording, `summarize_trip` for observed ranges and missing counts, and `read_trip_samples` to inspect individual moments. For example, ask “At the first recorded peak RPM, what were the elapsed time, speed, and coolant temperature? Cite the trip ID.” The summary gives the peak value. The sample rows identify when it occurred and which other measurements were available during that cycle.

A measured zero counts, including 0 km/h while parked. A missing sensor field appears as `null` in the sample tool. If all three measurements are unavailable, the logger saves no row for that cycle. A gap therefore has unknown vehicle activity, and missing counts describe only saved rows. The log does not preserve the reason a sensor value was missing. Reconnecting can start a new recording during the same drive, and an offline upload can arrive long after capture.
