#!/usr/bin/env python3
"""Integration tests for the bounded CFR relay recorder."""

from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


CFRR_MAGIC = 0x52524643
TYPE_RAW_DBR = 0x80000000
TYPE_SESSION_START = 0x80000003
TYPE_SESSION_END = 0x80000004
TYPE_REARM = 0x80000005
V1_HDR = struct.Struct("<IHHIIIIIIQ")
V2_HDR = struct.Struct("<IHHIIIIIIQQIIQ")
SESSION_START = struct.Struct("<HHIIIIIIIIIIQIII")
SESSION_START_V2_EXT = struct.Struct("<IIII")
SESSION_END = struct.Struct("<HHIIIQQQQQQQQ")
SOURCE = Path(__file__).with_name("cfr_relay_record.c")


def frame(
    payload: bytes,
    record_type: int,
    seq: int,
    *,
    version: int = 2,
    session_id: int = 1,
    header_extra: bytes = b"",
) -> bytes:
    if version == 1:
        return V1_HDR.pack(
            CFRR_MAGIC, 1, V1_HDR.size, record_type, 0, seq,
            len(payload), 0, 0, 123456789,
        ) + payload
    return V2_HDR.pack(
        CFRR_MAGIC, version, V2_HDR.size + len(header_extra), record_type,
        0, seq, len(payload), 0, 0, 123456789, session_id, 0, 0, 0,
    ) + header_extra + payload


def session_start_payload() -> bytes:
    return SESSION_START.pack(
        1, SESSION_START.size, 23, 0, 0x21, 7, 100, 10,
        32 * 1024, 128, 32 * 1024, 2, 123456789, 0, 0, 0,
    )


def session_start_v2_payload() -> bytes:
    base = bytearray(session_start_payload())
    struct.pack_into(
        "<HH", base, 0, 2, SESSION_START.size + SESSION_START_V2_EXT.size
    )
    return bytes(base) + SESSION_START_V2_EXT.pack(256, 1, 1, 250)


def session_end_payload(records_dropped: int = 0) -> bytes:
    return SESSION_END.pack(
        1, SESSION_END.size, 1, 3, 0, 4, 4, records_dropped,
        1024, 128, 7, 1, 223456789,
    )


class CfrRelayRecordTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build_dir = tempfile.TemporaryDirectory(prefix="cfr-recorder-build-")
        cls.binary = Path(cls.build_dir.name) / "cfr_relay_record"
        subprocess.run(
            [
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                str(SOURCE), "-o", str(cls.binary),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.build_dir.cleanup()

    def run_recorder(
        self, capture: bytes, *extra: str
    ) -> tuple[subprocess.CompletedProcess[str], bytes, dict[str, object], str]:
        with tempfile.TemporaryDirectory(prefix="cfr-recorder-test-") as tmp:
            root = Path(tmp)
            input_path = root / "input.cfrr"
            output_path = root / "output.cfrr"
            input_path.write_bytes(capture)
            result = subprocess.run(
                [
                    str(self.binary),
                    "--input", str(input_path),
                    "--output", str(output_path),
                    "--control", "/dev/null",
                    "--seconds", "0",
                    "--stats-seconds", "0",
                    "--no-follow",
                    *extra,
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            output = output_path.read_bytes() if output_path.exists() else b""
            metadata_path = Path(str(output_path) + ".json")
            metadata = (
                json.loads(metadata_path.read_text(encoding="utf-8"))
                if metadata_path.exists() else {}
            )
            checksum_path = Path(str(output_path) + ".sha256")
            checksum = (
                checksum_path.read_text(encoding="utf-8")
                if checksum_path.exists() else ""
            )
            return result, output, metadata, checksum

    def test_split_reads_resync_and_unknown_compatible_version(self) -> None:
        first = frame(b"legacy", TYPE_RAW_DBR, 7, version=1)
        second = frame(
            b"future", TYPE_RAW_DBR, 8, version=3,
            header_extra=b"future!!",
        )
        prefix = b"noise"
        result, output, metadata, checksum = self.run_recorder(
            prefix + first + second, "--read-size", "7"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, first + second)
        self.assertEqual(metadata["records"], 2)
        self.assertEqual(metadata["resync_bytes"], len(prefix))
        self.assertEqual(metadata["invalid_frames"], 0)
        self.assertEqual(checksum.split()[0], hashlib.sha256(output).hexdigest())
        self.assertEqual(metadata["segments"][0]["sha256"], hashlib.sha256(output).hexdigest())
        self.assertTrue(metadata["segments"][0]["complete"])

    def test_v2_lifecycle_sequence_gap_and_session_end_stop(self) -> None:
        capture = b"".join((
            frame(session_start_v2_payload(), TYPE_SESSION_START, 1, session_id=9),
            frame(b"raw", TYPE_RAW_DBR, 3, session_id=9),
            frame(session_end_payload(2), TYPE_SESSION_END, 4, session_id=9),
        ))
        result, output, metadata, _checksum = self.run_recorder(
            capture, "--stop-on-session-end"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, capture)
        self.assertEqual(metadata["sessions_seen"], 1)
        self.assertEqual(metadata["sequence_gaps"], 1)
        self.assertTrue(metadata["session_end_seen"])
        self.assertEqual(metadata["stop_reason"], "session_end")

    def test_invalid_session_end_does_not_stop(self) -> None:
        invalid_end = frame(
            struct.pack("<HH", 1, SESSION_END.size), TYPE_SESSION_END, 1
        )
        raw = frame(b"after-invalid-end", TYPE_RAW_DBR, 2)
        result, output, metadata, _checksum = self.run_recorder(
            invalid_end + raw, "--stop-on-session-end"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, invalid_end + raw)
        self.assertEqual(metadata["invalid_frames"], 1)
        self.assertFalse(metadata["session_end_seen"])
        self.assertEqual(metadata["stop_reason"], "eof")

    def test_record_limit_drains_buffered_session_end(self) -> None:
        start = frame(session_start_payload(), TYPE_SESSION_START, 1)
        raw1 = frame(b"one", TYPE_RAW_DBR, 2)
        raw2 = frame(b"two", TYPE_RAW_DBR, 3)
        end = frame(session_end_payload(), TYPE_SESSION_END, 4)
        result, output, metadata, _checksum = self.run_recorder(
            start + raw1 + raw2 + end,
            "--max-records", "2",
            "--relay-off-on-exit",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, start + raw1)
        self.assertEqual(metadata["stop_reason"], "max_records")
        self.assertEqual(metadata["records"], 2)
        self.assertEqual(metadata["drain_records_discarded"], 2)
        self.assertEqual(metadata["sequence_gaps"], 0)
        self.assertTrue(metadata["session_end_seen"])

    def test_unknown_version_session_end_is_opaque(self) -> None:
        unknown_end = frame(
            session_end_payload(), TYPE_SESSION_END, 1, version=3
        )
        raw = frame(b"after-unknown-end", TYPE_RAW_DBR, 2, version=3)
        result, output, metadata, _checksum = self.run_recorder(
            unknown_end + raw, "--stop-on-session-end"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, unknown_end + raw)
        self.assertEqual(metadata["invalid_frames"], 0)
        self.assertFalse(metadata["session_end_seen"])
        self.assertEqual(metadata["stop_reason"], "eof")

    def test_invalid_length_and_truncated_tail_are_bounded(self) -> None:
        malformed = V2_HDR.pack(
            CFRR_MAGIC, 2, V2_HDR.size, TYPE_RAW_DBR, 0, 1,
            40_000, 0, 0, 0, 1, 0, 0, 0,
        )
        valid = frame(b"valid", TYPE_RAW_DBR, 2)
        truncated = frame(b"truncated", TYPE_RAW_DBR, 3)[:-2]
        result, output, metadata, _checksum = self.run_recorder(
            malformed + valid + truncated, "--read-size", "13"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, valid)
        self.assertGreaterEqual(metadata["invalid_frames"], 1)
        self.assertEqual(metadata["trailing_incomplete_frames"], 1)
        self.assertGreater(metadata["trailing_buffer_bytes"], 0)

    def test_session_id_change_and_sequence_wrap(self) -> None:
        capture = b"".join((
            frame(b"a", TYPE_RAW_DBR, 0xFFFFFFFF, session_id=1),
            frame(b"b", TYPE_RAW_DBR, 0, session_id=1),
            frame(b"c", TYPE_RAW_DBR, 1, session_id=2),
        ))
        result, output, metadata, _checksum = self.run_recorder(capture)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, capture)
        self.assertEqual(metadata["sequence_gaps"], 0)
        self.assertEqual(metadata["sequence_resets"], 0)
        self.assertEqual(metadata["sessions_seen"], 2)
        self.assertEqual(metadata["session_changes"], 1)

    def test_stale_duplicate_does_not_amplify_gap(self) -> None:
        capture = b"".join(
            frame(b"", 0x90000001, seq, session_id=4)
            for seq in (1, 2, 1, 4)
        )
        result, output, metadata, _checksum = self.run_recorder(capture)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, capture)
        self.assertEqual(metadata["sequence_gaps"], 1)
        self.assertEqual(metadata["sequence_resets"], 1)
        self.assertEqual(metadata["out_of_order_records"], 1)

    def test_rejects_symlink_output(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cfr-recorder-path-") as tmp:
            root = Path(tmp)
            input_path = root / "input.cfrr"
            target = root / "target"
            output_path = root / "output.cfrr"
            input_path.write_bytes(frame(b"safe", TYPE_RAW_DBR, 1))
            target.write_bytes(b"do-not-touch")
            output_path.symlink_to(target)
            result = subprocess.run(
                [
                    str(self.binary), "--input", str(input_path),
                    "--output", str(output_path), "--control", "/dev/null",
                    "--seconds", "0", "--no-follow",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(target.read_bytes(), b"do-not-touch")

    def test_rejects_checksum_hardlink_to_input(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cfr-recorder-path-") as tmp:
            root = Path(tmp)
            input_path = root / "input.cfrr"
            output_path = root / "output.cfrr"
            checksum_path = Path(str(output_path) + ".sha256")
            capture = frame(b"safe", TYPE_RAW_DBR, 1)
            input_path.write_bytes(capture)
            os.link(input_path, checksum_path)
            result = subprocess.run(
                [
                    str(self.binary), "--input", str(input_path),
                    "--output", str(output_path), "--control", "/dev/null",
                    "--seconds", "0", "--no-follow",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(input_path.read_bytes(), capture)


if __name__ == "__main__":
    unittest.main()
