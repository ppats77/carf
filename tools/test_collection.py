"""Offline checks for MX+ prompt handling, PID discovery and a silent CAN capture."""
import unittest

from listen_can import frame, record, setup
from snapshot import bitmap_rows, survey


class Adapter:
    def __init__(self, responses):
        self.responses = responses
        self.commands = []
        self.pending = b""

    def write(self, command):
        command = command.decode("ascii").rstrip("\r")
        self.commands.append(command)
        assert command in self.responses, command
        self.pending = self.responses[command]

    @property
    def in_waiting(self):
        return len(self.pending)

    def read(self, size):
        out, self.pending = self.pending[:size], self.pending[size:]
        return out


class CollectionTests(unittest.TestCase):
    def test_discovery(self):
        settings = ("ATD", "ATE0", "ATL0", "ATH0", "ATS1", "ATSP0", "ATH1", "ATD0")
        replies = {cmd: b"OK\r>" for cmd in settings}
        replies.update({cmd: b"NO DATA\r>" for cmd in
                        ("0105", "010C", "0123", "03", "07", "0A", "020200", "0600", "0900")})
        replies.update({"ATDPN": b"A6\r>", "0100": b"41 00 08 10 00 01\r>",
                        "0120": b"7E8 06 41 20 20 00 00 00\r>"})
        port = Adapter(replies)
        def emit(row):
            if "protocol" in row:
                replies["0100"] = b"7E8 06 41 00 08 00 00 01\r7E9 06 41 00 00 10 00 00\r>"
            output.append(row)
        output = []
        survey(port, emit)
        advertised = [(r["responder"], r["advertised_pids"]) for r in output if "responder" in r]
        self.assertEqual(advertised, [("7E8", ["05"]), ("7E9", ["0C"]), ("7E8", ["23"])])
        self.assertEqual(port.commands.count("0123"), 1)
        self.assertNotIn("0140", port.commands)
        self.assertEqual(list(bitmap_rows("18 DA F1 10 06 41 00 80 00 00 00", "7", 0)),
                         [("18DAF110", 0x80000000)])

    def test_passive_only(self):
        settings = ("ATD", "ATE0", "ATL0", "STP 31", "STCMM 0", "ATH1", "ATS0",
                    "ATD0", "STFAC", "STFPA 00,00", "ATCF 000", "ATCM 000")
        port = Adapter({**{cmd:b"OK\r>" for cmd in settings},
                        "STM": b"7E803410550\r18DAF110A0FF\rBUFFER FULL\r>", " ": b""})
        setup(port)
        output = []
        self.assertEqual(record(port, output.append, 1), "adapter stopped")
        self.assertEqual((output[0]["can_id"], output[0]["data_hex"]), ("7E8", "03410550"))
        self.assertEqual(output[1]["can_id"], "18DAF110")
        self.assertEqual(output[2]["adapter_status"], "BUFFER FULL")
        self.assertIn("adapter_status", frame("NO DATA"))


if __name__ == "__main__":
    unittest.main()
