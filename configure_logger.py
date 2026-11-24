"""Provision Wi-Fi and the receiver on the workstation before flashing the Core2."""
from getpass import getpass
import json
import os
from pathlib import Path
from tempfile import NamedTemporaryFile
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent


def receiver_url(address):
    target = urlsplit("//" + address)
    if (not target.hostname or target.username is not None or target.password is not None
            or target.path or target.query or target.fragment or any(c.isspace() for c in address)):
        raise ValueError("Enter only the NAS hostname or LAN IP and optional port.")
    port = target.port if target.port is not None else 8787
    if not 1 <= port <= 65535:
        raise ValueError("Receiver port must be between 1 and 65535.")
    host = f"[{target.hostname}]" if ":" in target.hostname else target.hostname
    return f"http://{host}:{port}/v1/readings"


def saved_token():
    path = ROOT / ".env"
    if path.is_file():
        for line in path.read_text().splitlines():
            if line.startswith("CARF_UPLOAD_TOKEN="):
                return line.partition("=")[2]
    return ""


def main():
    dest = ROOT / "firmware/include/settings.h"
    if dest.exists() and input("Replace existing firmware settings? [y/N] ").lower() != "y":
        return
    ssid = input("Home 2.4 GHz Wi-Fi name: ")
    if not 1 <= len(ssid.encode()) <= 32:
        raise SystemExit("Wi-Fi name must be 1 to 32 bytes.")
    password = getpass("Wi-Fi password (hidden): ")
    url = receiver_url(input("Synology LAN hostname or IP, optionally host:port: ").strip())
    token = saved_token() or getpass("Receiver CARF_UPLOAD_TOKEN (hidden): ").strip()
    if not token:
        raise SystemExit("An upload token is required.")
    settings = {"CARF_WIFI_SSID": ssid, "CARF_WIFI_PASSWORD": password,
                "CARF_RECEIVER_URL": url, "CARF_UPLOAD_TOKEN": token,
                "CARF_OBD_NAME": input("Adapter name [OBDLink MX+]: ").strip() or "OBDLink MX+",
                "CARF_NTP": input("Clock server [pool.ntp.org]: ").strip() or "pool.ntp.org"}
    with NamedTemporaryFile("w", dir=dest.parent, delete=False, encoding="utf-8") as tmp:
        tmp.write("#pragma once\n")
        for key, value in settings.items():
            tmp.write(f"#define {key} {json.dumps(value, ensure_ascii=False)}\n")
    os.replace(tmp.name, dest)
    print("Saved private settings. Rebuild and flash the Core2 over USB.")


if __name__ == "__main__":
    main()
