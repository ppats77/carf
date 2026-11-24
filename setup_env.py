"""Create independent database and upload credentials for a fresh CARF deployment."""
import os
from pathlib import Path
import secrets


def credentials():
    keys = ("POSTGRES_PASSWORD", "WRITER_PASSWORD", "READER_PASSWORD", "CARF_UPLOAD_TOKEN")
    return {key: secrets.token_hex(32) for key in keys}


if __name__ == "__main__":
    try:
        fd = os.open(Path(__file__).with_name(".env"), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        raise SystemExit("Keeping existing .env")
    with os.fdopen(fd, "w") as env:
        for name, value in credentials().items():
            env.write(f"{name}={value}\n")
    print("Created .env with private database credentials and the firmware upload token.")
