"""Run the upload and MCP rehearsal in a unique local Compose project."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
from uuid import uuid4

from setup_env import credentials


def ask_claude(compose, project):
    # Only the container receives database credentials. Claude gets a local stdio command.
    name = project + "-reader"
    compose("run", "--rm", "-d", "--no-deps", "--name", name, "mcp", "sleep", "300")
    try:
        config = {"mcpServers": {"carf": {
            "command": "docker", "args": ["exec", "-i", name, "python", "mcp_server.py"],
        }}}
        prompt = ("Use CARF list_trips and summarize_trip for the most recently uploaded trip, "
                  "then read_trip_samples to locate the first sample at the observed peak RPM. "
                  "Return one JSON object with sample_count, peak_rpm, missing_rpm (count), "
                  "minimum_speed_kph, peak_elapsed_ms, and coolant_c_at_peak. "
                  "Use null for an unavailable measurement. Then list trips filtered to prius_2005, "
                  "read its battery observation (request 21CE), and report lowest_block_number "
                  "(one-based) and battery_spread_v in the same object.")
        fields = {
            "sample_count": {"type": "integer"}, "peak_rpm": {"type": "number"},
            "missing_rpm": {"type": "integer"}, "minimum_speed_kph": {"type": "integer"},
            "lowest_block_number": {"type": "integer"}, "battery_spread_v": {"type": "number"},
            "peak_elapsed_ms": {"type": "integer"}, "coolant_c_at_peak": {"type": ["integer", "null"]},
        }
        schema = {"type": "object", "properties": fields, "required": list(fields),
                  "additionalProperties": False}
        with TemporaryDirectory() as temp:
            result = subprocess.run(
                ["claude", "-p", "--no-session-persistence", "--strict-mcp-config",
                 "--mcp-config", json.dumps(config), "--tools", "",
                 "--allowedTools", "mcp__carf__list_trips,mcp__carf__summarize_trip,mcp__carf__read_trip_samples",
                 "--output-format", "json", "--json-schema", json.dumps(schema), "--", prompt],
                cwd=temp, text=True, stdout=subprocess.PIPE, timeout=180, check=True,
            )
        response = json.loads(result.stdout)
        answer = response.get("structured_output")
        if answer is None:
            raise RuntimeError(f"Claude did not return structured output: {response.get('result')}")
        expected = {
            "sample_count": 3, "peak_rpm": 1600, "missing_rpm": 1,
            "minimum_speed_kph": 0, "peak_elapsed_ms": 7000, "coolant_c_at_peak": None,
            "lowest_block_number": 1, "battery_spread_v": 0.13,
        }
        if answer != expected:
            raise RuntimeError(f"Unexpected Claude result: {answer}")
        print("Claude via MCP:", json.dumps(answer), flush=True)
    finally:
        subprocess.run(["docker", "stop", name], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--claude", action="store_true", help="also query the synthetic trip with Claude Code")
    args = parser.parse_args()
    project = "carf-check-" + uuid4().hex[:12]
    env = {**os.environ, **credentials(), "CARF_BIND": "127.0.0.1", "CARF_PORT": "0"}
    base = ["docker", "compose", "-p", project]

    def compose(*args, check=True):
        return subprocess.run(
            [*base, *args], check=check, env=env, cwd=Path(__file__).resolve().parent
        )

    try:
        compose("up", "--build", "-d", "--wait", "receiver")
        compose("run", "--rm", "-T", "--no-deps", "-e", "CARF_UPLOAD_TOKEN", "mcp",
                "python", "smoke_test.py")
        if args.claude:
            ask_claude(compose, project)
    finally:
        # This command targets only the unique throwaway project and its generated volume.
        compose("down", "--volumes", "--rmi", "local", check=True)


if __name__ == "__main__":
    main()
