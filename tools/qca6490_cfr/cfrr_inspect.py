#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Inspect a framed QCA6490 CFRR capture without external dependencies."""

import argparse
import hashlib
import json
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

MAGIC = 0x52524643
MAGIC_BYTES = b"CFRR"
V1_HEADER = struct.Struct("<IHHIIIIIIQ")
V2_EXTENSION = struct.Struct("<QIIQ")
DBR_META = struct.Struct("<IHH7I6HI8B2H14B15HI")
RX_PPDU = struct.Struct("<10I")
MAX_RECORD_SIZE = 32 * 1024

RECORD_NAMES = {
    0x00000000: "final",
    0x80000000: "raw_dbr",
    0x80000001: "rx_ppdu",
    0x80000002: "dbr_meta",
    0x80000003: "session_start",
    0x80000004: "session_end",
    0x80000005: "rearm",
}
REARM_NAMES = {1: "soft", 2: "hard", 3: "blind"}


@dataclass
class ParseStats:
    records: int = 0
    bytes: int = 0
    invalid_headers: int = 0
    resync_bytes: int = 0
    truncated_tail: int = 0
    sequence_gaps: int = 0
    out_of_order: int = 0
    lifecycle_errors: int = 0
    orphan_records: int = 0
    counter_mismatches: int = 0
    invalid_payloads: int = 0
    first_timestamp_ns: Optional[int] = None
    last_timestamp_ns: Optional[int] = None
    type_counts: Counter = field(default_factory=Counter)
    versions: Counter = field(default_factory=Counter)
    sessions: set = field(default_factory=set)
    expected_sequence: dict = field(default_factory=dict)
    session_states: dict = field(default_factory=dict)
    session_record_counts: Counter = field(default_factory=Counter)
    session_byte_counts: Counter = field(default_factory=Counter)
    active_session: Optional[int] = None
    rearms: Counter = field(default_factory=Counter)
    rearm_failures: int = 0
    session_ends: list = field(default_factory=list)


class CFRRReader:
    def __init__(self, stream, max_record_size=MAX_RECORD_SIZE):
        self.stream = stream
        self.max_record_size = max_record_size
        self.buffer = bytearray()
        self.base_offset = 0
        self.eof = False
        self.stats = ParseStats()

    def _fill(self, size):
        while len(self.buffer) < size and not self.eof:
            chunk = self.stream.read(max(65536, size - len(self.buffer)))
            if chunk:
                self.buffer.extend(chunk)
            else:
                self.eof = True
        return len(self.buffer) >= size

    def _discard(self, size, resync=True):
        del self.buffer[:size]
        self.base_offset += size
        if resync:
            self.stats.resync_bytes += size

    def records(self):
        while True:
            if not self._fill(V1_HEADER.size):
                if self.buffer:
                    self.stats.truncated_tail += 1
                return

            magic_offset = self.buffer.find(MAGIC_BYTES)
            if magic_offset < 0:
                keep = min(3, len(self.buffer))
                self._discard(len(self.buffer) - keep)
                continue
            if magic_offset:
                self._discard(magic_offset)
                continue

            values = V1_HEADER.unpack_from(self.buffer)
            (
                magic,
                version,
                header_len,
                record_type,
                flags,
                sequence,
                payload_len,
                meta0,
                meta1,
                timestamp_ns,
            ) = values
            total_len = header_len + payload_len
            minimum_header = 40 if version == 1 else 64

            if (
                magic != MAGIC
                or version == 0
                or header_len < minimum_header
                or header_len > self.max_record_size
                or total_len > self.max_record_size
                or total_len < header_len
            ):
                self.stats.invalid_headers += 1
                self._discard(1)
                continue

            if not self._fill(total_len):
                self.stats.truncated_tail += 1
                return

            session_id = 0
            pdev_id = 0
            if header_len >= 64:
                session_id, pdev_id, _reserved0, _reserved1 = (
                    V2_EXTENSION.unpack_from(self.buffer, V1_HEADER.size)
                )

            frame_offset = self.base_offset
            payload = bytes(self.buffer[header_len:total_len])
            frame = {
                "offset": frame_offset,
                "version": version,
                "header_len": header_len,
                "type": record_type,
                "type_name": RECORD_NAMES.get(record_type, hex(record_type)),
                "flags": flags,
                "sequence": sequence,
                "payload_len": payload_len,
                "meta0": meta0,
                "meta1": meta1,
                "timestamp_ns": timestamp_ns,
                "session_id": session_id,
                "pdev_id": pdev_id,
                "payload": payload,
            }
            self._account(frame, total_len)
            self._discard(total_len, resync=False)
            yield frame

    def _account(self, frame, total_len):
        stats = self.stats
        stats.records += 1
        stats.bytes += total_len
        stats.type_counts[frame["type_name"]] += 1
        stats.versions[str(frame["version"])] += 1
        stats.sessions.add(frame["session_id"])
        stats.session_record_counts[frame["session_id"]] += 1
        stats.session_byte_counts[frame["session_id"]] += total_len

        timestamp = frame["timestamp_ns"]
        if stats.first_timestamp_ns is None:
            stats.first_timestamp_ns = timestamp
        stats.last_timestamp_ns = timestamp

        session = frame["session_id"]
        sequence = frame["sequence"]
        expected = stats.expected_sequence.get(session)
        if expected is not None and sequence != expected:
            delta = (sequence - expected) & 0xFFFFFFFF
            if delta < 0x80000000:
                stats.sequence_gaps += delta
            else:
                stats.out_of_order += 1
                return
        stats.expected_sequence[session] = (sequence + 1) & 0xFFFFFFFF

        details = decode_payload(frame)
        frame["details"] = details
        if frame["type_name"] in {
            "dbr_meta",
            "rx_ppdu",
            "session_start",
            "session_end",
            "rearm",
        } and details is None:
            stats.invalid_payloads += 1
        if frame["type_name"] == "rearm" and details:
            stage = details.get("stage_name", "unknown")
            stats.rearms[stage] += 1
            if details.get("status"):
                stats.rearm_failures += 1
        if frame["type_name"] == "session_end" and details:
            stats.session_ends.append(details)

        name = frame["type_name"]
        if name == "session_start":
            if (
                stats.active_session is not None
                or frame["session_id"] in stats.session_states
            ):
                stats.lifecycle_errors += 1
            else:
                stats.active_session = frame["session_id"]
                stats.session_states[frame["session_id"]] = "active"
        elif name == "session_end":
            valid_end = (
                stats.active_session == frame["session_id"]
                and stats.session_states.get(frame["session_id"]) == "active"
            )
            if not valid_end:
                stats.lifecycle_errors += 1
            else:
                stats.session_states[frame["session_id"]] = "ended"
                stats.active_session = None
                if details:
                    expected_last = (frame["sequence"] - 1) & 0xFFFFFFFF
                    if (
                        details["last_sequence"] != expected_last
                        or details["records_attempted"]
                        != details["records_committed"]
                        + details["records_dropped"]
                        or details["records_committed"]
                        != stats.session_record_counts[frame["session_id"]]
                        or details["bytes_committed"]
                        != stats.session_byte_counts[frame["session_id"]]
                    ):
                        stats.counter_mismatches += 1
        elif stats.active_session != frame["session_id"]:
            stats.orphan_records += 1


def decode_payload(frame):
    payload = frame["payload"]
    name = frame["type_name"]

    if name == "dbr_meta" and len(payload) >= DBR_META.size:
        values = DBR_META.unpack_from(payload)
        if values[0] != 0x4D524244 or values[1] != 1:
            return None
        raw_header_len = values[56]
        if values[2] != DBR_META.size or raw_header_len > len(payload) - DBR_META.size:
            return None
        return {
            "version": values[1],
            "meta_len": values[2],
            "flags": values[3],
            "cookie": values[4],
            "phy_ppdu_id": values[5],
            "physical_address": values[6] | (values[7] << 32),
            "dbr_len": values[8],
            "parsed_len": values[9],
            "dma_header_bytes": values[10],
            "dma_header_words": values[11],
            "freeze_tlv_len": values[12],
            "mu_rx_user_size": values[13],
            "mu_rx_num_users": values[14],
            "sample_offset": values[15],
            "sample_len": values[16],
            "tag": values[17],
            "capture_type": values[19],
            "preamble_type": values[20],
            "nss": values[21],
            "num_chains": values[22],
            "upload_bandwidth": values[23],
            "sw_peer_id": values[25],
            "total_bytes": values[26],
            "header_version": values[27],
            "target_id": values[28],
            "cfr_format": values[29],
            "decimation_factor": values[33],
            "raw_header_len": raw_header_len,
        }

    if name == "rx_ppdu" and len(payload) >= RX_PPDU.size:
        if len(payload) != RX_PPDU.size:
            return None
        values = RX_PPDU.unpack_from(payload)
        if values[0] != 0x50505243:
            return None
        return {
            "ppdu_id": values[1],
            "bb_captured_channel": values[2],
            "rx_location_info_valid": values[3],
            "capture_status": values[4],
            "rtt_buffer_pointer": values[5] | (values[6] << 32),
            "buffer_address": values[7] | (values[8] << 32),
            "srng_id": values[9],
        }

    if name == "session_start" and len(payload) >= 64:
        base = struct.unpack_from("<HH10IQ3I", payload)
        version = base[0]
        declared_len = base[1]
        minimum_len = 64 if version == 1 else 80
        if (
            version == 0
            or declared_len != len(payload)
            or declared_len < minimum_len
            or (version == 1 and declared_len != 64)
            or (version == 2 and declared_len != 80)
        ):
            return None
        result = {
            "version": version,
            "payload_len": declared_len,
            "chip_type": base[2],
            "pdev_id": base[3],
            "capture_mode": base[4],
            "filter_group_bitmap": base[5],
            "capture_duration_us": base[6],
            "capture_interval_us": base[7],
            "relay_subbuf_size": base[8],
            "relay_num_subbufs": base[9],
            "max_record_size": base[10],
            "framing_version": base[11],
            "start_timestamp_ns": base[12],
        }
        if len(payload) >= 80:
            count, mode, continuous, stall = struct.unpack_from("<4I", payload, 64)
            result.update(
                capture_count=count,
                capture_interval_mode=mode,
                continuous_enabled=continuous,
                watchdog_stall_ms=stall,
            )
        return result

    if name == "session_end" and len(payload) >= 80:
        values = struct.unpack_from("<HHIII8Q", payload)
        if values[0] != 1 or values[1] != 80 or len(payload) != 80:
            return None
        if values[2] < 1 or values[2] > 6:
            return None
        return {
            "version": values[0],
            "payload_len": values[1],
            "stop_reason": values[2],
            "last_sequence": values[3],
            "records_attempted": values[5],
            "records_committed": values[6],
            "records_dropped": values[7],
            "bytes_committed": values[8],
            "bytes_dropped": values[9],
            "correlation_successes": values[10],
            "correlation_misses": values[11],
            "end_timestamp_ns": values[12],
        }

    if name == "rearm" and len(payload) >= 60:
        values = struct.unpack_from("<HHII4Q4I", payload)
        if (
            values[0] != 1
            or values[1] != 60
            or len(payload) != 60
            or values[2] not in REARM_NAMES
        ):
            return None
        return {
            "version": values[0],
            "payload_len": values[1],
            "stage": values[2],
            "stage_name": REARM_NAMES.get(values[2], "unknown"),
            "status": values[3],
            "epoch": values[4],
            "last_ppdu_timestamp_ns": values[5],
            "last_dbr_timestamp_ns": values[6],
            "rearm_timestamp_ns": values[7],
            "stall_threshold_ms": values[8],
            "poll_interval_ms": values[9],
            "drain_interval_ms": values[10],
        }

    return None


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def summary(path, stats):
    duration_ms = 0.0
    if stats.first_timestamp_ns is not None and stats.last_timestamp_ns is not None:
        duration_ms = (stats.last_timestamp_ns - stats.first_timestamp_ns) / 1_000_000
    return {
        "path": str(path),
        "sha256": file_sha256(path),
        "records": stats.records,
        "bytes": stats.bytes,
        "duration_ms": round(duration_ms, 3),
        "type_counts": dict(sorted(stats.type_counts.items())),
        "versions": dict(sorted(stats.versions.items())),
        "session_ids": sorted(stats.sessions),
        "session_states": dict(sorted(stats.session_states.items())),
        "sequence_gaps": stats.sequence_gaps,
        "out_of_order": stats.out_of_order,
        "lifecycle_errors": stats.lifecycle_errors,
        "orphan_records": stats.orphan_records,
        "counter_mismatches": stats.counter_mismatches,
        "invalid_headers": stats.invalid_headers,
        "invalid_payloads": stats.invalid_payloads,
        "resync_bytes": stats.resync_bytes,
        "truncated_tail": stats.truncated_tail,
        "rearms": dict(sorted(stats.rearms.items())),
        "rearm_failures": stats.rearm_failures,
        "session_ends": stats.session_ends,
    }


def print_text(result):
    print(f"file: {result['path']}")
    print(f"sha256: {result['sha256']}")
    print(f"records: {result['records']}")
    print(f"bytes: {result['bytes']}")
    print(f"duration_ms: {result['duration_ms']}")
    print(f"types: {json.dumps(result['type_counts'], sort_keys=True)}")
    print(f"versions: {json.dumps(result['versions'], sort_keys=True)}")
    print(f"sessions: {len(result['session_ids'])}")
    print(f"sequence_gaps: {result['sequence_gaps']}")
    print(f"out_of_order: {result['out_of_order']}")
    print(f"lifecycle_errors: {result['lifecycle_errors']}")
    print(f"orphan_records: {result['orphan_records']}")
    print(f"counter_mismatches: {result['counter_mismatches']}")
    print(f"invalid_headers: {result['invalid_headers']}")
    print(f"invalid_payloads: {result['invalid_payloads']}")
    print(f"resync_bytes: {result['resync_bytes']}")
    print(f"truncated_tail: {result['truncated_tail']}")
    print(f"rearms: {json.dumps(result['rearms'], sort_keys=True)}")
    print(f"rearm_failures: {result['rearm_failures']}")
    if result["session_ends"]:
        print(
            "session_end: "
            + json.dumps(result["session_ends"][-1], sort_keys=True)
        )


def result_has_errors(result, allow_incomplete=False):
    states = result.get("session_states", {})
    incomplete = not allow_incomplete and (
        not states
        or any(state != "ended" for state in states.values())
        or result["orphan_records"]
    )
    dropped = any(end.get("records_dropped", 0) for end in result["session_ends"])
    return bool(
        not result["records"]
        or result["invalid_headers"]
        or result["invalid_payloads"]
        or result["resync_bytes"]
        or result["truncated_tail"]
        or result["sequence_gaps"]
        or result["out_of_order"]
        or result["lifecycle_errors"]
        or result["counter_mismatches"]
        or result["rearm_failures"]
        or incomplete
        or dropped
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument(
        "--allow-incomplete",
        action="store_true",
        help="allow a rotated segment without matching session boundaries",
    )
    parser.add_argument(
        "--records",
        type=int,
        default=0,
        help="print details for the first N records before the summary",
    )
    args = parser.parse_args()

    if args.records < 0:
        parser.error("--records must be non-negative")

    with args.capture.open("rb") as stream:
        reader = CFRRReader(stream)
        for index, frame in enumerate(reader.records()):
            if index < args.records:
                details = decode_payload(frame)
                visible = {key: value for key, value in frame.items() if key != "payload"}
                if details:
                    visible["details"] = details
                print(json.dumps(visible, sort_keys=True))

    result = summary(args.capture, reader.stats)
    if args.as_json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print_text(result)

    return 1 if result_has_errors(result, args.allow_incomplete) else 0


if __name__ == "__main__":
    sys.exit(main())
