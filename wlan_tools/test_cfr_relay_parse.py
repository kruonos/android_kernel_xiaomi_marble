#!/usr/bin/env python3
"""Unit tests for CFR relay parsing and CSI sample decode."""

from __future__ import annotations

import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path


PARSER_PATH = Path(__file__).with_name("cfr_relay_parse.py")
spec = importlib.util.spec_from_file_location("cfr_relay_parse", PARSER_PATH)
assert spec and spec.loader
cfr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cfr)


def build_raw_payload() -> bytes:
    # 12 words total header: 4-word enhanced header + 7-word freeze TLV +
    # 1-word alignment. Four IQ samples: two tones, two RX chains.
    hdr_words = 12
    total_bytes = 16
    w0 = 0xBA | (hdr_words << 8)
    w1 = (
        1  # upload_done
        | (1 << 1)  # capture_type RTT-H
        | (2 << 4)  # VHT
        | (0 << 6)  # NSS encoded 0 => 1 stream
        | (1 << 9)  # chains encoded 1 => 2 chains
        | (2 << 12)  # 80 MHz
        | (1 << 15)  # sw_peer_id_valid
    )
    w5 = 2 | (3 << 4) | (0 << 8) | (1 << 11)  # raw 32-bit + freeze TLV
    raw_header = struct.pack("<HHHHHHHH", w0, w1, 8, 0x1234, total_bytes, w5, 0, 0)

    freeze = struct.pack(
        "<14H",
        1 | (5 << 1) | (2 << 4) | (8 << 6),
        8,
        0x1234,
        0x1122,
        0x3344,
        0x5566,
        0xaabb,
        0xccdd,
        0xeeff,
        1,
        2,
        3,
        4,
        0,
    )
    padding = b"\0" * 4
    samples = struct.pack("<hhhhhhhh", 1, -2, 3, -4, 5, -6, 7, -8)
    return raw_header + freeze + padding + samples


def build_frame(payload: bytes, record_type: int = cfr.TYPE_RAW_DBR) -> bytes:
    header = cfr.CFRR_HDR.pack(
        cfr.CFRR_MAGIC,
        1,
        cfr.CFRR_HDR.size,
        record_type,
        0,
        7,
        len(payload),
        9,
        len(payload),
        123456789,
    )
    return header + payload


def build_v2_frame(
    payload: bytes,
    record_type: int,
    *,
    seq: int,
    session_id: int = 0x1122334455667788,
    version: int = 2,
    header_extra: bytes = b"",
) -> bytes:
    header = cfr.CFRR_V2_HDR.pack(
        cfr.CFRR_MAGIC,
        version,
        cfr.CFRR_V2_HDR.size + len(header_extra),
        record_type,
        0,
        seq,
        len(payload),
        9,
        len(payload),
        987654321,
        session_id,
        0,
        0,
        0,
    )
    return header + header_extra + payload


def build_session_start_payload() -> bytes:
    return cfr.SESSION_START_V1.pack(
        1,
        cfr.SESSION_START_V1.size,
        23,
        0,
        0x21,
        0x7,
        100,
        10,
        32 * 1024,
        128,
        32 * 1024,
        2,
        123456789,
        0,
        0,
        0,
    )


def build_session_start_v2_payload() -> bytes:
    base = bytearray(build_session_start_payload())
    struct.pack_into("<HH", base, 0, 2, cfr.SESSION_START_V1.size +
                     cfr.SESSION_START_V2_EXT.size)
    return bytes(base) + cfr.SESSION_START_V2_EXT.pack(256, 1, 1, 250)


def build_session_end_payload(records_dropped: int = 2) -> bytes:
    return cfr.SESSION_END_V1.pack(
        1,
        cfr.SESSION_END_V1.size,
        1,
        3,
        0,
        4,
        4,
        records_dropped,
        1024,
        128,
        7,
        1,
        223456789,
    )


def build_rearm_payload(stage: int = 1, status: int = 0, epoch: int = 1) -> bytes:
    return cfr.REARM_V1.pack(
        1, cfr.REARM_V1.size, stage, status, epoch,
        100_000_000, 90_000_000, 350_000_000, 250, 50, 75, 0,
    )


def build_final_payload(raw: bytes) -> bytes:
    common = struct.pack(
        "<IIBBBBIQ",
        cfr.CSI_START,
        0x001374,
        1,
        1,
        23,
        0,
        0,
        123456789,
    )
    return common + raw + struct.pack("<I", cfr.CSI_END)


def build_dbr_meta_payload() -> bytes:
    raw = build_raw_payload()
    raw_header = raw[:48]
    values = [
        cfr.DBR_META_MAGIC, 1, cfr.DBR_META_V1.size, 0x3B, 9, 0x1234,
        0x11223344, 0x5, len(raw), len(raw), 16, 12, 28, 0, 0, 48, 16,
        0xBA, 1, 1, 2, 0, 1, 2, 1, 8, 16, 2, 3, 0, 0, 1, 0, 0, 0,
        1, 5, 2, 8, 0, 1, 8, 0x1234,
        0x1122, 0x3344, 0x5566, 0xaabb, 0xccdd, 0xeeff,
        1, 2, 3, 4, 0, 0, 0, len(raw_header),
    ]
    return cfr.DBR_META_V1.pack(*values) + raw_header


class CfrRelayParseTest(unittest.TestCase):
    def test_parse_raw_header_and_tlvs(self) -> None:
        payload = build_raw_payload()
        raw = cfr.parse_raw_dbr_payload(payload)
        self.assertIsNotNone(raw)
        assert raw is not None
        self.assertEqual(raw["tag"], 0xBA)
        self.assertEqual(raw["sample_offset"], 48)
        self.assertEqual(raw["parsed_len"], 64)
        self.assertEqual(raw["bandwidth_mhz"], 80)
        self.assertEqual(raw["stream_count"], 1)
        self.assertEqual(raw["chain_count"], 2)
        self.assertEqual(raw["tone_count_per_lane"], 2)
        self.assertEqual(raw["freeze_tlv"]["packet_ta"], "22:11:44:33:66:55")

    def test_decode_iq_samples(self) -> None:
        payload = build_raw_payload()
        raw = cfr.parse_raw_dbr_payload(payload)
        assert raw is not None
        decoded = cfr.decode_cfr_samples(payload, raw, iq_layout="iq16-le")
        self.assertEqual(decoded["summary"]["sample_count"], 4)
        self.assertEqual(decoded["summary"]["lane_count"], 2)
        first = decoded["samples"][0]
        self.assertEqual((first["i"], first["q"]), (1, -2))
        self.assertEqual((first["tone_index"], first["lane_index"], first["chain_index"]), (0, 0, 0))
        second = decoded["samples"][1]
        self.assertEqual((second["tone_index"], second["lane_index"], second["chain_index"]), (0, 1, 1))

    def test_parse_framed_record(self) -> None:
        frame = build_frame(build_raw_payload())
        records = cfr.parse_framed(frame)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["record_type"], "raw_dbr")
        self.assertEqual(records[0]["seq"], 7)
        self.assertEqual(records[0]["raw_dbr"]["phy_ppdu_id"], 0x1234)

    def test_truncated_frame(self) -> None:
        frame = build_frame(build_raw_payload())[:-5]
        records = cfr.parse_framed(frame)
        self.assertEqual(records[0]["record_type"], "truncated_frame")
        self.assertGreater(records[0]["declared_payload_len"], records[0]["available"])

    def test_parse_dbr_meta_record(self) -> None:
        payload = build_dbr_meta_payload()
        meta = cfr.parse_dbr_meta(payload)
        self.assertIsNotNone(meta)
        assert meta is not None
        self.assertEqual(meta["cookie"], 9)
        self.assertEqual(meta["phy_ppdu_id"], 0x1234)
        self.assertEqual(meta["bandwidth_mhz"], 80)
        self.assertEqual(meta["packet_ta"], "22:11:44:33:66:55")
        self.assertIn("raw_header_bytes", meta["flag_names"])
        self.assertEqual(meta["raw_header_decoded"]["sample_offset"], 48)

        records = cfr.parse_framed(build_frame(payload, cfr.TYPE_DBR_META))
        self.assertEqual(records[0]["record_type"], "dbr_meta")
        self.assertEqual(records[0]["dbr_meta"]["tone_count_per_lane"], 2)

    def test_parse_v2_session_lifecycle_and_gap(self) -> None:
        capture = b"".join((
            build_v2_frame(
                build_session_start_payload(), cfr.TYPE_SESSION_START, seq=1
            ),
            build_v2_frame(build_raw_payload(), cfr.TYPE_RAW_DBR, seq=3),
            build_v2_frame(
                build_session_end_payload(), cfr.TYPE_SESSION_END, seq=4
            ),
        ))
        records = cfr.parse_framed(capture)
        self.assertEqual(len(records), 3)
        self.assertEqual(records[0]["hdr_len"], 64)
        self.assertEqual(records[0]["session_id"], 0x1122334455667788)
        self.assertTrue(records[0]["header_reserved_zero"])
        self.assertTrue(records[0]["session_start"]["valid"])
        self.assertEqual(records[0]["session_start"]["relay_num_subbufs"], 128)
        self.assertTrue(records[2]["session_end"]["valid"])
        self.assertEqual(records[2]["session_end"]["stop_reason_name"], "capture_stop")

        framing = cfr.analyze_framing(records)
        self.assertEqual(framing["sessions"], 1)
        self.assertEqual(framing["sequence_gaps"], 1)
        self.assertEqual(framing["sequence_resets"], 0)
        self.assertEqual(framing["kernel_reported_drops"], 2)

    def test_parse_continuous_session_and_rearm(self) -> None:
        capture = b"".join((
            build_v2_frame(
                build_session_start_v2_payload(), cfr.TYPE_SESSION_START,
                seq=1, session_id=7,
            ),
            build_v2_frame(
                build_rearm_payload(), cfr.TYPE_REARM, seq=2, session_id=7
            ),
            build_v2_frame(
                build_rearm_payload(stage=3, epoch=2), cfr.TYPE_REARM,
                seq=3, session_id=7,
            ),
            build_v2_frame(
                build_session_end_payload(), cfr.TYPE_SESSION_END,
                seq=4, session_id=7,
            ),
        ))
        records = cfr.parse_framed(capture)
        session = records[0]["session_start"]
        self.assertTrue(session["valid"])
        self.assertEqual(session["version"], 2)
        self.assertEqual(session["capture_count"], 256)
        self.assertEqual(session["capture_interval_mode"], 1)
        self.assertEqual(session["continuous_enabled"], 1)
        self.assertEqual(session["watchdog_stall_ms"], 250)
        self.assertTrue(records[1]["rearm"]["valid"])
        self.assertEqual(records[1]["rearm"]["stage_name"], "soft")
        self.assertEqual(records[2]["rearm"]["stage_name"], "blind")
        framing = cfr.analyze_framing(records)
        self.assertEqual(framing["rearm_records"], 2)
        self.assertEqual(framing["soft_rearms"], 1)
        self.assertEqual(framing["blind_rearms"], 1)
        self.assertEqual(framing["rearm_failures"], 0)

    def test_unknown_compatible_version_extension(self) -> None:
        frame = build_v2_frame(
            build_raw_payload(),
            cfr.TYPE_RAW_DBR,
            seq=1,
            version=3,
            header_extra=b"future!!",
        )
        records = cfr.parse_framed(frame)
        self.assertEqual(len(records), 1)
        self.assertFalse(records[0]["known_version"])
        self.assertFalse(records[0]["compatible_version"])
        self.assertTrue(records[0]["forward_skippable"])
        self.assertEqual(records[0]["header_extension_len"], 8)
        self.assertNotIn("raw_dbr", records[0])
        self.assertEqual(
            records[0]["opaque_payload_sha256"],
            cfr.sha256_hex(build_raw_payload()),
        )

    def test_truncated_v2_fixed_header(self) -> None:
        frame = build_v2_frame(build_raw_payload(), cfr.TYPE_RAW_DBR, seq=1)
        records = cfr.parse_framed(frame[:52])
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["record_type"], "truncated_header")
        self.assertEqual(records[0]["available"], 52)

    def test_stale_duplicate_does_not_amplify_sequence_gap(self) -> None:
        capture = b"".join(
            build_v2_frame(b"", 0x90000001, seq=seq, session_id=3)
            for seq in (1, 2, 1, 4)
        )
        framing = cfr.analyze_framing(cfr.parse_framed(capture))
        self.assertEqual(framing["sequence_gaps"], 1)
        self.assertEqual(framing["sequence_resets"], 1)
        self.assertEqual(framing["out_of_order_records"], 1)

    def test_monotonic_v2_timestamp_is_not_wall_clock_stale(self) -> None:
        records = cfr.parse_framed(
            build_v2_frame(build_raw_payload(), cfr.TYPE_RAW_DBR, seq=1)
        )
        cfr.annotate_stale(records, {
            "start_real_ns": 10_000_000_000,
            "end_real_ns": 20_000_000_000,
        })
        self.assertFalse(records[0]["stale"])
        self.assertEqual(records[0]["metadata_window"], "not_comparable_monotonic")

    def test_auto_export_keeps_finals_and_unmatched_raw(self) -> None:
        matched_raw = build_raw_payload()
        unmatched_raw = matched_raw[:-1] + bytes([matched_raw[-1] ^ 0x7F])
        capture = b"".join((
            build_v2_frame(matched_raw, cfr.TYPE_RAW_DBR, seq=1),
            build_v2_frame(
                build_final_payload(matched_raw), cfr.TYPE_FINAL, seq=2
            ),
            build_v2_frame(unmatched_raw, cfr.TYPE_RAW_DBR, seq=3),
        ))
        records_with_payload = list(cfr.iter_framed_payloads(capture))
        selected = cfr.select_export_records(records_with_payload, "auto")
        self.assertEqual(
            [record["record_type"] for record, _payload in selected],
            ["final", "raw_dbr"],
        )

    def test_auto_export_does_not_deduplicate_across_sessions(self) -> None:
        raw = build_raw_payload()
        capture = b"".join((
            build_v2_frame(raw, cfr.TYPE_RAW_DBR, seq=1, session_id=1),
            build_v2_frame(
                build_final_payload(raw), cfr.TYPE_FINAL, seq=1, session_id=2
            ),
        ))
        selected = cfr.select_export_records(
            list(cfr.iter_framed_payloads(capture)), "auto"
        )
        self.assertEqual(
            [record["record_type"] for record, _payload in selected],
            ["raw_dbr", "final"],
        )

    def test_unframed_final_at_nonzero_offset(self) -> None:
        prefix = b"prefix" + struct.pack("<I", cfr.CSI_END)
        final = build_final_payload(build_raw_payload())
        records = cfr.parse_unframed(prefix + final)
        finals = [record for record in records if record["record_type"] == "final"]
        raws = [record for record in records if record["record_type"] == "raw_dbr"]
        self.assertEqual(len(finals), 1)
        self.assertEqual(finals[0]["offset"], len(prefix))
        self.assertEqual(raws, [])

    def test_rotated_segment_metadata_discovery_and_validation(self) -> None:
        data = build_v2_frame(build_raw_payload(), cfr.TYPE_RAW_DBR, seq=1)
        with tempfile.TemporaryDirectory(prefix="cfr-parser-metadata-") as tmp:
            root = Path(tmp)
            capture = root / "capture.000000.cfrr"
            metadata_path = root / "capture.json"
            capture.write_bytes(data)
            metadata_path.write_text(json.dumps({
                "exit_status": 0,
                "segments": [{
                    "path": str(capture),
                    "bytes": len(data),
                    "complete": True,
                    "sha256": cfr.sha256_hex(data),
                }],
            }), encoding="utf-8")

            metadata = cfr.load_capture_metadata(capture, None)
            self.assertIsNotNone(metadata)
            validation = cfr.validate_capture_metadata(capture, data, metadata)
            self.assertEqual(validation["status"], "ok")


if __name__ == "__main__":
    unittest.main()
