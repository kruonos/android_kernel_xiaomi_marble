#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import io
import struct
import unittest

import cfrr_inspect as cfr


def frame(
    record_type,
    sequence,
    payload=b"",
    session_id=7,
    timestamp=100,
    version=2,
    header_len=64,
    reserved0=0,
    reserved1=0,
):
    base = struct.pack(
        "<IHHIIIIIIQ",
        cfr.MAGIC,
        version,
        header_len,
        record_type,
        0,
        sequence,
        len(payload),
        0,
        0,
        timestamp,
    )
    if header_len == 40:
        return base + payload
    extension = struct.pack(
        "<QIIQ",
        session_id,
        0,
        reserved0,
        reserved1,
    )
    return base + extension + payload


def session_start():
    return struct.pack(
        "<HH10IQ3I4I",
        2,
        80,
        6,
        0,
        0x20,
        0xFFFF,
        100000,
        100000,
        32768,
        128,
        32768,
        2,
        100,
        0,
        0,
        0,
        256,
        0,
        1,
        250,
    )


def rearm():
    return struct.pack(
        "<HHII4Q4I",
        1,
        60,
        3,
        0,
        35,
        90,
        80,
        110,
        250,
        50,
        75,
        0,
    )


def session_end():
    return struct.pack(
        "<HHIII8Q",
        1,
        80,
        1,
        2,
        0,
        3,
        3,
        0,
        412,
        0,
        1,
        0,
        200,
    )


def result_from(reader):
    stats = reader.stats
    return {
        "records": stats.records,
        "type_counts": dict(stats.type_counts),
        "session_states": dict(stats.session_states),
        "invalid_headers": stats.invalid_headers,
        "invalid_payloads": stats.invalid_payloads,
        "resync_bytes": stats.resync_bytes,
        "truncated_tail": stats.truncated_tail,
        "sequence_gaps": stats.sequence_gaps,
        "out_of_order": stats.out_of_order,
        "lifecycle_errors": stats.lifecycle_errors,
        "orphan_records": stats.orphan_records,
        "counter_mismatches": stats.counter_mismatches,
        "rearm_failures": stats.rearm_failures,
        "session_ends": stats.session_ends,
    }


class CFRRInspectTest(unittest.TestCase):
    def parse(self, data):
        reader = cfr.CFRRReader(io.BytesIO(data))
        records = list(reader.records())
        return reader, records

    def test_session_rearm_and_end(self):
        data = b"".join(
            [
                frame(0x80000003, 1, session_start(), timestamp=100),
                frame(0x80000005, 2, rearm(), timestamp=110),
                frame(0x80000004, 3, session_end(), timestamp=200),
            ]
        )
        reader, records = self.parse(data)

        self.assertEqual(reader.stats.records, 3)
        self.assertEqual(reader.stats.sequence_gaps, 0)
        self.assertEqual(reader.stats.rearms["blind"], 1)
        self.assertEqual(reader.stats.rearm_failures, 0)
        self.assertEqual(len(reader.stats.session_ends), 1)
        self.assertEqual(cfr.decode_payload(records[0])["capture_count"], 256)
        self.assertEqual(cfr.decode_payload(records[1])["epoch"], 35)
        self.assertEqual(cfr.decode_payload(records[2])["records_dropped"], 0)

        result = result_from(reader)
        self.assertFalse(cfr.result_has_errors(result))

    def test_sequence_gap_is_counted(self):
        reader, _ = self.parse(frame(0, 1) + frame(0, 4))
        self.assertEqual(reader.stats.sequence_gaps, 2)

    def test_resync_and_truncated_tail_are_counted(self):
        reader, records = self.parse(b"bad" + frame(0, 1) + b"CFRR")
        self.assertEqual(len(records), 1)
        self.assertEqual(reader.stats.resync_bytes, 3)
        self.assertEqual(reader.stats.truncated_tail, 1)

    def test_v2_requires_full_header(self):
        reader, records = self.parse(frame(0, 1, version=2, header_len=40))
        self.assertEqual(records, [])
        self.assertGreater(reader.stats.invalid_headers, 0)

    def test_future_version_and_reserved_fields_are_opaque(self):
        reader, records = self.parse(
            frame(0, 1, version=3, reserved0=0x1234, reserved1=0x5678)
        )
        self.assertEqual(len(records), 1)
        self.assertEqual(reader.stats.invalid_headers, 0)

    def test_sequence_rollover(self):
        data = b"".join(
            [frame(0, 0xFFFFFFFE), frame(0, 0xFFFFFFFF), frame(0, 0)]
        )
        reader, _ = self.parse(data)
        self.assertEqual(reader.stats.sequence_gaps, 0)
        self.assertEqual(reader.stats.out_of_order, 0)

    def test_oversize_record_is_rejected(self):
        payload = b"\0" * cfr.MAX_RECORD_SIZE
        reader, records = self.parse(frame(0, 1, payload))
        self.assertEqual(records, [])
        self.assertGreater(reader.stats.invalid_headers, 0)

    def test_dbr_meta_and_rx_ppdu_decode(self):
        meta = [0] * 57
        meta[0] = 0x4D524244
        meta[1] = 1
        meta[2] = cfr.DBR_META.size
        meta[4] = 12
        meta[5] = 34
        meta[16] = 432
        meta[21] = 2
        meta[22] = 2
        meta[33] = 1
        dbr_payload = cfr.DBR_META.pack(*meta)
        rx_payload = cfr.RX_PPDU.pack(
            0x50505243, 34, 1, 1, 0, 0x11223344, 0x5, 0x55667788, 0x6, 0
        )
        _, records = self.parse(
            frame(0x80000002, 1, dbr_payload)
            + frame(0x80000001, 2, rx_payload)
        )
        dbr = cfr.decode_payload(records[0])
        rx = cfr.decode_payload(records[1])
        self.assertEqual(dbr["cookie"], 12)
        self.assertEqual(dbr["phy_ppdu_id"], 34)
        self.assertEqual(dbr["sample_len"], 432)
        self.assertEqual(rx["ppdu_id"], 34)
        self.assertEqual(rx["rtt_buffer_pointer"], 0x511223344)

    def test_incomplete_capture_fails_by_default(self):
        reader, _ = self.parse(frame(0, 1))
        result = result_from(reader)
        self.assertTrue(cfr.result_has_errors(result))
        self.assertFalse(cfr.result_has_errors(result, allow_incomplete=True))

    def test_session_ids_must_match(self):
        reader, _ = self.parse(
            frame(0x80000003, 1, session_start(), session_id=7)
            + frame(0x80000004, 2, session_end(), session_id=8)
        )
        self.assertGreater(reader.stats.lifecycle_errors, 0)
        self.assertTrue(cfr.result_has_errors(result_from(reader)))

    def test_session_end_cannot_precede_start(self):
        reader, _ = self.parse(
            frame(0x80000004, 1, session_end(), session_id=7)
            + frame(0x80000003, 2, session_start(), session_id=7)
        )
        self.assertGreater(reader.stats.lifecycle_errors, 0)

    def test_overlapping_session_is_rejected(self):
        reader, _ = self.parse(
            frame(0x80000003, 1, session_start(), session_id=7)
            + frame(0x80000003, 1, session_start(), session_id=8)
        )
        self.assertGreater(reader.stats.lifecycle_errors, 0)

    def test_declared_payload_length_is_validated(self):
        payload = bytearray(session_start())
        struct.pack_into("<H", payload, 2, 0xFFFF)
        reader, _ = self.parse(frame(0x80000003, 1, payload))
        self.assertEqual(reader.stats.invalid_payloads, 1)

    def test_v2_session_start_rejects_extensions(self):
        payload = bytearray(session_start()) + b"\0"
        struct.pack_into("<H", payload, 2, len(payload))
        reader, _ = self.parse(frame(0x80000003, 1, payload))
        self.assertEqual(reader.stats.invalid_payloads, 1)

    def test_session_counters_are_reconciled(self):
        payload = bytearray(session_end())
        struct.pack_into("<Q", payload, 40, 999)
        reader, _ = self.parse(
            frame(0x80000003, 1, session_start())
            + frame(0x80000005, 2, rearm())
            + frame(0x80000004, 3, payload)
        )
        self.assertEqual(reader.stats.counter_mismatches, 1)

    def test_invalid_rearm_stage_is_rejected(self):
        payload = bytearray(rearm())
        struct.pack_into("<I", payload, 4, 99)
        reader, _ = self.parse(frame(0x80000005, 1, payload))
        self.assertEqual(reader.stats.invalid_payloads, 1)


if __name__ == "__main__":
    unittest.main()
