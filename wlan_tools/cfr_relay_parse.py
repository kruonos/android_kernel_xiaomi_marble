#!/usr/bin/env python3
"""Summarize and decode Qualcomm CFR/CSI relay captures.

The parser understands the framed stream produced by the local CFR relay patch
and falls back to best-effort marker scanning for older unframed captures.

For QCA6490/HSP enhanced CFR captures, raw DBR payloads are decoded as:

    whal_cfir_enhanced_hdr + optional freeze/MU TLVs + CFR sample bytes

The kernel source documents the header/TLV layout and that cfr_fmt=0 uses
4 bytes per tone. It does not document final lane/subcarrier ordering, so CSV
and SVG exports carry the selected I/Q and lane-order assumptions explicitly.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import html
import json
import math
import re
import struct
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


CFRR_MAGIC = 0x52524643
CFRR_MAGIC_BYTES = struct.pack("<I", CFRR_MAGIC)
CFRR_V1_HDR = struct.Struct("<IHHIIIIIIQ")
CFRR_V2_HDR = struct.Struct("<IHHIIIIIIQQIIQ")
# Kept as an alias for callers that construct legacy v1 fixtures.
CFRR_HDR = CFRR_V1_HDR
CFRR_MAX_HDR_LEN = 256

TYPE_FINAL = 0
TYPE_RAW_DBR = 0x80000000
TYPE_RX_PPDU = 0x80000001
TYPE_DBR_META = 0x80000002
TYPE_SESSION_START = 0x80000003
TYPE_SESSION_END = 0x80000004
TYPE_REARM = 0x80000005

TYPE_NAMES = {
    TYPE_FINAL: "final",
    TYPE_RAW_DBR: "raw_dbr",
    TYPE_RX_PPDU: "rx_ppdu",
    TYPE_DBR_META: "dbr_meta",
    TYPE_SESSION_START: "session_start",
    TYPE_SESSION_END: "session_end",
    TYPE_REARM: "rearm",
}
NAME_TYPES = {value: key for key, value in TYPE_NAMES.items()}

SESSION_START_V1 = struct.Struct("<HHIIIIIIIIIIQIII")
SESSION_START_V2_EXT = struct.Struct("<IIII")
SESSION_END_V1 = struct.Struct("<HHIIIQQQQQQQQ")
REARM_V1 = struct.Struct("<HHIIQQQQIIII")
STOP_REASON_NAMES = {
    1: "capture_stop",
    2: "relay_disabled",
    3: "relay_reset",
    4: "restart",
    5: "error",
    6: "shutdown",
}

CSI_START = 0xDEADBEAF
CSI_END = 0xBEAFDEAD
CSI_START_BYTES = struct.pack("<I", CSI_START)
CSI_END_BYTES = struct.pack("<I", CSI_END)
RX_PPDU_MAGIC = 0x50505243
DBR_META_MAGIC = 0x4D524244
DBR_META_V1 = struct.Struct(
    "<IHH" + "I" * 7 + "H" * 6 + "I" + "B" * 8 + "HH" +
    "B" * 8 + "B" * 6 + "HH" + "H" * 13 + "I"
)

RAW_ENH_HDR_LEN = 16
ENH_METADATA_LEN = 136
CSI_COMMON_LEN = 24

BW_MHZ = {
    0: 20,
    1: 40,
    2: 80,
    3: 160,
}
PREAMBLE_NAMES = {
    0: "legacy",
    1: "ht",
    2: "vht",
    3: "he",
}
CAPTURE_TYPE_NAMES = {
    0: "none",
    1: "rtt-h",
    2: "debug-h",
    5: "rtt-h-cir",
}
CFR_FORMAT_NAMES = {
    0: "raw_32bit_iq_candidate",
    1: "compressed_24bit_undocumented",
}
FREEZE_TLV_NAMES = {
    0: "v1_compat_or_unspecified",
    1: "v1_hsp_cypress",
    2: "v2_maple_spruce_moselle",
    3: "v3_pine",
    4: "v4_hamilton",
    5: "v5_waikiki",
}
FREEZE_CAPTURE_REASONS = {
    0: "tm",
    1: "ftm",
    2: "ack_resp_to_tm_ftm",
    3: "ta_ra_type_filter",
    4: "ndp_ndp",
    5: "all_packet",
}
PACKET_TYPES = {
    0: "management",
    1: "control",
    2: "data",
    3: "extension",
}

SVG_COLORS = [
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#d62728",
    "#9467bd",
    "#8c564b",
    "#e377c2",
    "#7f7f7f",
]


class DecodeError(ValueError):
    """Raised when a CFR payload cannot be decoded safely."""


def sha256_hex(buf: bytes) -> str:
    return hashlib.sha256(buf).hexdigest()


def mac_from_words(lower: int, mid: int, upper: int) -> str:
    data = [
        lower & 0xFF,
        (lower >> 8) & 0xFF,
        mid & 0xFF,
        (mid >> 8) & 0xFF,
        upper & 0xFF,
        (upper >> 8) & 0xFF,
    ]
    return ":".join(f"{byte:02x}" for byte in data)


def parse_raw_dbr_header(buf: bytes, off: int = 0) -> dict[str, Any] | None:
    if off + RAW_ENH_HDR_LEN > len(buf):
        return None

    w0, w1, sw_peer, phy, total, w5, w6, rsvd = struct.unpack_from(
        "<HHHHHHHH", buf, off
    )
    tag = w0 & 0xFF
    hdr_words = (w0 >> 8) & 0x3F
    sample_offset = hdr_words * 4
    parsed_len = sample_offset + total

    upload_done = w1 & 1
    capture_type = (w1 >> 1) & 7
    preamble = (w1 >> 4) & 3
    nss = (w1 >> 6) & 7
    chains = (w1 >> 9) & 7
    bw = (w1 >> 12) & 7
    sw_peer_id_valid = (w1 >> 15) & 1

    header_version = w5 & 0xF
    target_id = (w5 >> 4) & 0xF
    cfr_fmt = (w5 >> 8) & 1
    rsvd2 = (w5 >> 9) & 1
    mu_rx_data_incl = (w5 >> 10) & 1
    freeze_data_incl = (w5 >> 11) & 1
    freeze_tlv_version = (w5 >> 12) & 0xF

    mu_rx_num_users = w6 & 0xFF
    decimation_factor = (w6 >> 8) & 0xF
    rsvd3 = (w6 >> 12) & 0xF

    stream_count = nss + 1
    chain_count = chains + 1
    complex_sample_bytes = 4 if cfr_fmt == 0 else 3
    complex_sample_count = None
    tone_count_per_lane = None
    sample_remainder = None
    lane_count = stream_count * chain_count
    if complex_sample_bytes:
        complex_sample_count = total // complex_sample_bytes
        sample_remainder = total % complex_sample_bytes
        if lane_count > 0 and sample_remainder == 0:
            tone_count_per_lane = complex_sample_count // lane_count
            if complex_sample_count % lane_count:
                tone_count_per_lane = None

    available = len(buf) - off
    tail_len = None
    tail_nonzero = None
    if parsed_len <= available:
        tail_len = available - parsed_len
        tail_nonzero = any(buf[off + parsed_len:])

    return {
        "tag": tag,
        "hdr_words": hdr_words,
        "header_bytes": sample_offset,
        "sample_offset": sample_offset,
        "upload_done": upload_done,
        "capture_type": capture_type,
        "capture_type_name": CAPTURE_TYPE_NAMES.get(capture_type, "unknown"),
        "preamble": preamble,
        "preamble_name": PREAMBLE_NAMES.get(preamble, "unknown"),
        "nss": nss,
        "stream_count": stream_count,
        "chains": chains,
        "chain_count": chain_count,
        "lane_count": lane_count,
        "bw": bw,
        "bandwidth_mhz": BW_MHZ.get(bw),
        "sw_peer_id_valid": sw_peer_id_valid,
        "sw_peer_id": sw_peer,
        "phy_ppdu_id": phy,
        "total_bytes": total,
        "sample_bytes": total,
        "parsed_len": parsed_len,
        "valid_len": parsed_len,
        "word5": w5,
        "word6": w6,
        "reserved": rsvd,
        "header_version": header_version,
        "target_id": target_id,
        "cfr_fmt": cfr_fmt,
        "cfr_format": CFR_FORMAT_NAMES.get(cfr_fmt, "unknown"),
        "reserved2": rsvd2,
        "mu_rx_data_incl": mu_rx_data_incl,
        "freeze_data_incl": freeze_data_incl,
        "freeze_tlv_version": freeze_tlv_version,
        "freeze_tlv_name": FREEZE_TLV_NAMES.get(freeze_tlv_version, "unknown"),
        "mu_rx_num_users": mu_rx_num_users,
        "decimation_factor": decimation_factor,
        "reserved3": rsvd3,
        "complex_sample_bytes": complex_sample_bytes,
        "complex_sample_count": complex_sample_count,
        "sample_remainder_bytes": sample_remainder,
        "tone_count_per_lane": tone_count_per_lane,
        "tail_len": tail_len,
        "tail_nonzero": tail_nonzero,
    }


def raw_dbr_is_sane(header: dict[str, Any] | None, remaining: int) -> bool:
    if not header:
        return False
    if header["tag"] != 0xBA:
        return False
    if not 1 <= header["hdr_words"] <= 87:
        return False
    if not 0 < header["parsed_len"] <= remaining:
        return False
    if header["total_bytes"] == 0:
        return False
    if header["capture_type"] not in (1, 2, 5):
        return False
    if header["sample_remainder_bytes"]:
        return False
    return True


def freeze_tlv_len(version: int) -> int:
    if version == 3:
        return 30
    if version == 5:
        return 34
    return 28


def parse_freeze_tlv(buf: bytes, off: int, version: int) -> dict[str, Any] | None:
    size = freeze_tlv_len(version)
    if off + size > len(buf):
        return None

    count = size // 2
    words = struct.unpack_from("<" + "H" * count, buf, off)
    w0 = words[0]
    out: dict[str, Any] = {
        "version": version,
        "version_name": FREEZE_TLV_NAMES.get(version, "unknown"),
        "byte_len": size,
        "freeze": w0 & 1,
        "capture_reason": (w0 >> 1) & 7,
        "capture_reason_name": FREEZE_CAPTURE_REASONS.get((w0 >> 1) & 7, "unknown"),
        "packet_type": (w0 >> 4) & 3,
        "packet_type_name": PACKET_TYPES.get((w0 >> 4) & 3, "unknown"),
        "packet_sub_type": (w0 >> 6) & 0xF,
        "sw_peer_id_valid": (w0 >> 15) & 1,
        "sw_peer_id": words[1],
        "phy_ppdu_id": words[2],
        "packet_ta": mac_from_words(words[3], words[4], words[5]),
        "packet_ra": mac_from_words(words[6], words[7], words[8]),
        "packet_ta_words": [words[3], words[4], words[5]],
        "packet_ra_words": [words[6], words[7], words[8]],
    }

    if version == 3:
        out["directed"] = (w0 >> 10) & 1
        out["tsf_or_user_mask_36_32"] = words[12]
        out["tsf_timestamp"] = (
            words[9]
            | (words[10] << 16)
            | (words[11] << 32)
            | (words[12] << 48)
        )
        out["user_index_or_user_mask_15_0"] = words[13]
        out["user_mask_31_16"] = words[14]
    elif version == 5:
        out["directed"] = (words[13] >> 6) & 1
        out["tsf_timestamp"] = (
            words[9]
            | (words[10] << 16)
            | (words[11] << 32)
            | (words[12] << 48)
        )
        out["user_index_or_user_mask_5_0"] = words[13] & 0x3F
        out["user_mask_21_6"] = words[14]
        out["user_mask_36_22"] = words[15] & 0x7FFF
    else:
        out["directed"] = (words[13] >> 6) & 1
        out["tsf_timestamp"] = (
            words[9]
            | (words[10] << 16)
            | (words[11] << 32)
            | (words[12] << 48)
        )
        out["user_index_or_user_mask_5_0"] = words[13] & 0x3F

    return out


def parse_mu_user(buf: bytes, off: int, version: int) -> dict[str, Any] | None:
    if version == 5:
        if off + 8 > len(buf):
            return None
        w0, w1 = struct.unpack_from("<II", buf, off)
        return {
            "byte_len": 8,
            "bw_info_valid": w0 & 1,
            "uplink_receive_type": (w0 >> 1) & 3,
            "uplink_11ax_mcs": (w0 >> 4) & 0xF,
            "nss": (w0 >> 8) & 7,
            "stream_offset": (w0 >> 11) & 7,
            "sta_dcm": (w0 >> 14) & 1,
            "sta_coding": (w0 >> 15) & 1,
            "ru_type_80_0": (w0 >> 16) & 0xF,
            "ru_type_80_1": (w0 >> 20) & 0xF,
            "ru_type_80_2": (w0 >> 24) & 0xF,
            "ru_type_80_3": (w0 >> 28) & 0xF,
            "ru_start_index_80_0": w1 & 0x3F,
            "ru_start_index_80_1": (w1 >> 8) & 0x3F,
            "ru_start_index_80_2": (w1 >> 16) & 0x3F,
            "ru_start_index_80_3": (w1 >> 24) & 0x3F,
        }

    if off + 4 > len(buf):
        return None
    w0 = struct.unpack_from("<I", buf, off)[0]
    return {
        "byte_len": 4,
        "bw_info_valid": w0 & 1,
        "uplink_receive_type": (w0 >> 1) & 3,
        "uplink_11ax_mcs": (w0 >> 4) & 0xF,
        "ru_width": (w0 >> 8) & 0x7F,
        "nss": (w0 >> 16) & 7,
        "stream_offset": (w0 >> 19) & 7,
        "sta_dcm": (w0 >> 22) & 1,
        "sta_coding": (w0 >> 23) & 1,
        "ru_start_index": (w0 >> 24) & 0x7F,
    }


def parse_raw_dbr_payload(buf: bytes, off: int = 0) -> dict[str, Any] | None:
    header = parse_raw_dbr_header(buf, off)
    if not header:
        return None

    out = dict(header)
    out["payload_sha256"] = sha256_hex(buf[off:])
    if raw_dbr_is_sane(header, len(buf) - off):
        sample_start = off + header["sample_offset"]
        sample_end = sample_start + header["sample_bytes"]
        out["sample_sha256"] = sha256_hex(buf[sample_start:sample_end])
        out["sample_preview_hex"] = buf[sample_start:sample_start + 32].hex()

    cursor = off + RAW_ENH_HDR_LEN
    if header["freeze_data_incl"]:
        freeze = parse_freeze_tlv(buf, cursor, header["freeze_tlv_version"])
        out["freeze_tlv"] = freeze
        if freeze:
            cursor += freeze["byte_len"]

    mu_users = []
    if header["mu_rx_data_incl"]:
        for _ in range(header["mu_rx_num_users"]):
            user = parse_mu_user(buf, cursor, header["freeze_tlv_version"])
            if not user:
                break
            mu_users.append(user)
            cursor += user["byte_len"]
    if mu_users:
        out["mu_users"] = mu_users

    header_end = off + header["sample_offset"]
    if cursor <= header_end:
        padding = buf[cursor:header_end]
        out["header_padding_len"] = len(padding)
        out["header_padding_nonzero"] = any(padding)
    else:
        out["header_overrun_bytes"] = cursor - header_end

    return out


def parse_rx_ppdu_snapshot(buf: bytes, off: int = 0) -> dict[str, Any] | None:
    if off + 40 > len(buf):
        return None
    vals = struct.unpack_from("<IIIIIIIIII", buf, off)
    names = [
        "magic",
        "ppdu_id",
        "bb_captured_channel",
        "rx_location_info_valid",
        "chan_capture_status",
        "rtt_ptr_low32",
        "rtt_ptr_high8",
        "buffer_addr_low32",
        "buffer_addr_high32",
        "srng_id",
    ]
    out = dict(zip(names, vals))
    out["magic"] = f"0x{out['magic']:08x}"
    addr40 = out["rtt_ptr_low32"] | ((out["rtt_ptr_high8"] & 0xF) << 32)
    out["rtt_buffer_addr_40"] = f"0x{addr40:010x}"
    return out


def parse_dbr_meta(buf: bytes, off: int = 0) -> dict[str, Any] | None:
    if off + DBR_META_V1.size > len(buf):
        return None

    vals = DBR_META_V1.unpack_from(buf, off)
    names = [
        "magic", "version", "meta_len", "flags", "cookie",
        "phy_ppdu_id", "paddr_low32", "paddr_high32", "dbr_len",
        "parsed_len", "dma_hdr_bytes", "dma_hdr_words",
        "freeze_tlv_len", "mu_rx_user_size", "mu_rx_num_users",
        "sample_offset", "sample_len", "tag", "upload_done",
        "capture_type", "preamble_type", "nss", "num_chains",
        "upload_pkt_bw", "sw_peer_id_valid", "sw_peer_id", "total_bytes",
        "header_version", "target_id", "cfr_fmt", "mu_rx_data_incl",
        "freeze_data_incl", "freeze_tlv_version", "decimation_factor",
        "reserved0", "freeze", "freeze_capture_reason",
        "freeze_packet_type", "freeze_packet_subtype", "freeze_directed",
        "freeze_sw_peer_id_valid", "freeze_sw_peer_id",
        "freeze_phy_ppdu_id", "packet_ta_lower_16", "packet_ta_mid_16",
        "packet_ta_upper_16", "packet_ra_lower_16", "packet_ra_mid_16",
        "packet_ra_upper_16", "tsf_word0", "tsf_word1", "tsf_word2",
        "tsf_word3_or_user_mask_36_32", "user_mask_word0",
        "user_mask_word1", "user_mask_word2", "raw_header_len",
    ]
    out = dict(zip(names, vals))
    if out["magic"] != DBR_META_MAGIC:
        return None

    out["magic"] = f"0x{out['magic']:08x}"
    out["paddr"] = f"0x{(out['paddr_low32'] | (out['paddr_high32'] << 32)):016x}"
    out["bandwidth_mhz"] = BW_MHZ.get(out["upload_pkt_bw"])
    out["preamble_name"] = PREAMBLE_NAMES.get(out["preamble_type"], "unknown")
    out["capture_type_name"] = CAPTURE_TYPE_NAMES.get(out["capture_type"], "unknown")
    out["cfr_format"] = CFR_FORMAT_NAMES.get(out["cfr_fmt"], "unknown")
    out["freeze_tlv_name"] = FREEZE_TLV_NAMES.get(out["freeze_tlv_version"], "unknown")
    out["freeze_capture_reason_name"] = FREEZE_CAPTURE_REASONS.get(
        out["freeze_capture_reason"], "unknown"
    )
    out["freeze_packet_type_name"] = PACKET_TYPES.get(
        out["freeze_packet_type"], "unknown"
    )
    out["stream_count"] = out["nss"] + 1
    out["chain_count"] = out["num_chains"] + 1
    out["lane_count"] = out["stream_count"] * out["chain_count"]
    out["packet_ta"] = mac_from_words(
        out["packet_ta_lower_16"], out["packet_ta_mid_16"],
        out["packet_ta_upper_16"]
    )
    out["packet_ra"] = mac_from_words(
        out["packet_ra_lower_16"], out["packet_ra_mid_16"],
        out["packet_ra_upper_16"]
    )
    out["tsf_timestamp"] = (
        out["tsf_word0"] |
        (out["tsf_word1"] << 16) |
        (out["tsf_word2"] << 32) |
        (out["tsf_word3_or_user_mask_36_32"] << 48)
    )
    complex_sample_bytes = 4 if out["cfr_fmt"] == 0 else 3
    out["complex_sample_bytes"] = complex_sample_bytes
    out["complex_sample_count"] = out["sample_len"] // complex_sample_bytes
    out["sample_remainder_bytes"] = out["sample_len"] % complex_sample_bytes
    if out["lane_count"] and out["sample_remainder_bytes"] == 0:
        if out["complex_sample_count"] % out["lane_count"] == 0:
            out["tone_count_per_lane"] = out["complex_sample_count"] // out["lane_count"]
        else:
            out["tone_count_per_lane"] = None
    else:
        out["tone_count_per_lane"] = None

    flags = int(out["flags"])
    out["flag_names"] = [
        name for bit, name in (
            (0x00000001, "raw_hdr_valid"),
            (0x00000002, "freeze_present"),
            (0x00000004, "mu_present"),
            (0x00000008, "sample_present"),
            (0x00000010, "freeze_fields"),
            (0x00000020, "raw_header_bytes"),
        ) if flags & bit
    ]

    raw_header_len = int(out["raw_header_len"])
    raw_header_off = off + DBR_META_V1.size
    raw_header_end = raw_header_off + raw_header_len
    if raw_header_len and raw_header_end <= len(buf):
        raw_header_bytes = buf[raw_header_off:raw_header_end]
        out["raw_header_bytes_len"] = len(raw_header_bytes)
        out["raw_header_bytes_sha256"] = sha256_hex(raw_header_bytes)
        out["raw_header_preview_hex"] = raw_header_bytes[:32].hex()
        out["raw_header_decoded"] = parse_raw_dbr_payload(raw_header_bytes)

    return out


def parse_enh_cfr_metadata(buf: bytes, off: int = CSI_COMMON_LEN) -> dict[str, Any] | None:
    if off + ENH_METADATA_LEN > len(buf):
        return None

    (
        status,
        capture_bw,
        channel_bw,
        phy_mode,
        prim20_chan,
        center_freq1,
        center_freq2,
        capture_mode,
        capture_type,
        sts_count,
        num_rx_chain,
        timestamp,
        length,
        is_mu_ppdu,
        num_mu_users,
    ) = struct.unpack_from("<BBBBHHHBBBBQIBB", buf, off)
    pos = off + 28
    peer_blob = buf[pos:pos + 24]
    pos += 24
    chain_rssi = list(struct.unpack_from("<8I", buf, pos))
    pos += 32
    chain_phase = list(struct.unpack_from("<8H", buf, pos))
    pos += 16
    rtt_cfo_measurement = struct.unpack_from("<I", buf, pos)[0]
    pos += 4
    agc_gain = list(struct.unpack_from("<8B", buf, pos))
    pos += 8
    rx_start_ts = struct.unpack_from("<I", buf, pos)[0]
    pos += 4
    mcs_rate, gi_type = struct.unpack_from("<HH", buf, pos)
    pos += 4
    coding, stbc, beamformed, dcm, ltf_size, sgi, sig_reserved = struct.unpack_from(
        "<BBBBBBH", buf, pos
    )
    pos += 8
    agc_gain_tbl_index = list(struct.unpack_from("<8B", buf, pos))

    su_peer = ":".join(f"{byte:02x}" for byte in peer_blob[:6])
    mu_peers = [
        ":".join(f"{byte:02x}" for byte in peer_blob[i:i + 6])
        for i in range(0, 24, 6)
    ]

    return {
        "status": status,
        "capture_bw": capture_bw,
        "capture_bw_mhz": BW_MHZ.get(capture_bw),
        "channel_bw": channel_bw,
        "channel_bw_mhz": BW_MHZ.get(channel_bw),
        "phy_mode": phy_mode,
        "prim20_chan": prim20_chan,
        "center_freq1": center_freq1,
        "center_freq2": center_freq2,
        "capture_mode": capture_mode,
        "capture_type": capture_type,
        "sts_count": sts_count,
        "num_rx_chain": num_rx_chain,
        "timestamp": timestamp,
        "length": length,
        "is_mu_ppdu": is_mu_ppdu,
        "num_mu_users": num_mu_users,
        "su_peer_addr": su_peer,
        "mu_peer_addr": mu_peers,
        "chain_rssi": chain_rssi,
        "chain_phase": chain_phase,
        "rtt_cfo_measurement": rtt_cfo_measurement,
        "agc_gain": agc_gain,
        "rx_start_ts": rx_start_ts,
        "mcs_rate": mcs_rate,
        "gi_type": gi_type,
        "sig_info": {
            "coding": coding,
            "stbc": stbc,
            "beamformed": beamformed,
            "dcm": dcm,
            "ltf_size": ltf_size,
            "sgi": sgi,
            "reserved": sig_reserved,
        },
        "agc_gain_tbl_index": agc_gain_tbl_index,
    }


def parse_csi_common(buf: bytes, off: int = 0) -> dict[str, Any] | None:
    if off + CSI_COMMON_LEN > len(buf):
        return None
    start, vendor, meta_ver, data_ver, chip, platform, meta_len, host_ts = (
        struct.unpack_from("<IIBBBBIQ", buf, off)
    )
    if start != CSI_START:
        return None

    header_len = CSI_COMMON_LEN + meta_len
    raw_off = off + header_len
    end_off = None
    raw_header = None
    payload_len = None
    trailer_ok = False
    if raw_off + RAW_ENH_HDR_LEN <= len(buf):
        raw_header = parse_raw_dbr_header(buf, raw_off)
        if raw_dbr_is_sane(raw_header, len(buf) - raw_off):
            payload_len = raw_header["parsed_len"]
            expected_end = raw_off + payload_len
            if expected_end + 4 <= len(buf):
                trailer = struct.unpack_from("<I", buf, expected_end)[0]
                if trailer == CSI_END:
                    end_off = expected_end - off
                    trailer_ok = True

    if end_off is None and raw_off <= len(buf):
        candidate = buf.find(CSI_END_BYTES, raw_off)
        if candidate >= 0:
            end_off = candidate - off

    enh_metadata = None
    if meta_len >= ENH_METADATA_LEN:
        enh_metadata = parse_enh_cfr_metadata(buf, off + CSI_COMMON_LEN)

    return {
        "start_magic": f"0x{start:08x}",
        "vendor": f"0x{vendor:x}",
        "metadata_version": meta_ver,
        "data_version": data_ver,
        "chip_type": chip,
        "platform_type": platform,
        "metadata_len": meta_len,
        "header_len": header_len,
        "host_timestamp_ns": host_ts,
        "payload_len_from_raw_header": payload_len,
        "end_magic_offset": end_off,
        "end_magic_ok_at_expected_offset": trailer_ok,
        "enh_metadata": enh_metadata,
        "raw_payload_header": parse_raw_dbr_payload(buf, raw_off) if raw_header else None,
    }


def parse_session_start(buf: bytes, off: int = 0) -> dict[str, Any]:
    out: dict[str, Any] = {"valid": False, "available_len": len(buf) - off}
    if off + 4 > len(buf):
        out["error"] = "truncated_payload_prefix"
        return out
    version, payload_len = struct.unpack_from("<HH", buf, off)
    out.update({"version": version, "declared_payload_len": payload_len})
    if version not in (1, 2):
        out["error"] = "unsupported_payload_version"
        return out
    minimum_len = (
        SESSION_START_V1.size + SESSION_START_V2_EXT.size
        if version == 2 else SESSION_START_V1.size
    )
    if payload_len < minimum_len:
        out["error"] = "invalid_declared_payload_len"
        return out
    if payload_len > len(buf) - off or off + SESSION_START_V1.size > len(buf):
        out["error"] = "truncated_payload"
        return out

    values = SESSION_START_V1.unpack_from(buf, off)
    names = (
        "version", "declared_payload_len", "chip_type", "pdev_id",
        "capture_mode", "filter_group_bitmap", "capture_duration",
        "capture_interval", "relay_subbuf_size", "relay_num_subbufs",
        "max_record_size", "framing_version", "start_timestamp_ns",
        "reserved0", "reserved1", "reserved2",
    )
    out.update(zip(names, values))
    if version == 2:
        (capture_count, capture_interval_mode, continuous_enabled,
         watchdog_stall_ms) = SESSION_START_V2_EXT.unpack_from(
            buf, off + SESSION_START_V1.size
        )
        out.update({
            "capture_count": capture_count,
            "capture_interval_mode": capture_interval_mode,
            "continuous_enabled": continuous_enabled,
            "watchdog_stall_ms": watchdog_stall_ms,
        })
    out["valid"] = True
    return out


def parse_session_end(buf: bytes, off: int = 0) -> dict[str, Any]:
    out: dict[str, Any] = {"valid": False, "available_len": len(buf) - off}
    if off + 4 > len(buf):
        out["error"] = "truncated_payload_prefix"
        return out
    version, payload_len = struct.unpack_from("<HH", buf, off)
    out.update({"version": version, "declared_payload_len": payload_len})
    if version != 1:
        out["error"] = "unsupported_payload_version"
        return out
    if payload_len < SESSION_END_V1.size:
        out["error"] = "invalid_declared_payload_len"
        return out
    if payload_len > len(buf) - off or off + SESSION_END_V1.size > len(buf):
        out["error"] = "truncated_payload"
        return out

    values = SESSION_END_V1.unpack_from(buf, off)
    names = (
        "version", "declared_payload_len", "stop_reason", "last_sequence",
        "reserved0", "records_attempted", "records_committed",
        "records_dropped", "bytes_committed", "bytes_dropped",
        "correlation_successes", "correlation_misses", "end_timestamp_ns",
    )
    out.update(zip(names, values))
    out["stop_reason_name"] = STOP_REASON_NAMES.get(
        out["stop_reason"], "unknown"
    )
    out["valid"] = True
    return out


def parse_rearm(buf: bytes, off: int = 0) -> dict[str, Any]:
    out: dict[str, Any] = {"valid": False, "available_len": len(buf) - off}
    if off + 4 > len(buf):
        out["error"] = "truncated_payload_prefix"
        return out
    version, payload_len = struct.unpack_from("<HH", buf, off)
    out.update({"version": version, "declared_payload_len": payload_len})
    if version != 1:
        out["error"] = "unsupported_payload_version"
        return out
    if payload_len < REARM_V1.size:
        out["error"] = "invalid_declared_payload_len"
        return out
    if payload_len > len(buf) - off or off + REARM_V1.size > len(buf):
        out["error"] = "truncated_payload"
        return out
    values = REARM_V1.unpack_from(buf, off)
    names = (
        "version", "declared_payload_len", "stage", "status", "epoch",
        "last_ppdu_timestamp_ns", "last_dbr_timestamp_ns",
        "rearm_timestamp_ns", "stall_threshold_ms", "poll_interval_ms",
        "drain_interval_ms", "reserved0",
    )
    out.update(zip(names, values))
    out["stage_name"] = {1: "soft", 2: "hard", 3: "blind"}.get(
        out["stage"], "unknown"
    )
    out["valid"] = True
    return out


def summarize_payload(record_type: int, payload: bytes) -> dict[str, Any]:
    if record_type == TYPE_RAW_DBR:
        return {"raw_dbr": parse_raw_dbr_payload(payload, 0)}
    if record_type == TYPE_RX_PPDU:
        return {"rx_ppdu": parse_rx_ppdu_snapshot(payload, 0)}
    if record_type == TYPE_DBR_META:
        return {"dbr_meta": parse_dbr_meta(payload, 0)}
    if record_type == TYPE_SESSION_START:
        return {"session_start": parse_session_start(payload, 0)}
    if record_type == TYPE_SESSION_END:
        return {"session_end": parse_session_end(payload, 0)}
    if record_type == TYPE_REARM:
        return {"rearm": parse_rearm(payload, 0)}
    if record_type == TYPE_FINAL:
        csi = parse_csi_common(payload, 0)
        if csi:
            return {"csi_cfr": csi}
        raw = parse_raw_dbr_payload(payload, 0)
        if raw_dbr_is_sane(raw, len(payload)):
            return {"raw_dbr": raw}
    return {}


def iter_framed_payloads(buf: bytes) -> Iterable[tuple[dict[str, Any], bytes]]:
    off = 0
    while off + CFRR_V1_HDR.size <= len(buf):
        magic = struct.unpack_from("<I", buf, off)[0]
        if magic != CFRR_MAGIC:
            next_off = buf.find(CFRR_MAGIC_BYTES, off + 1)
            if next_off < 0:
                break
            off = next_off
            continue

        (magic, version, hdr_len, record_type, flags, seq, payload_len,
         meta0, meta1, timestamp_ns) = CFRR_V1_HDR.unpack_from(buf, off)
        minimum_hdr_len = CFRR_V2_HDR.size if version >= 2 else CFRR_V1_HDR.size
        if (version == 0 or hdr_len < minimum_hdr_len or
                hdr_len > CFRR_MAX_HDR_LEN):
            off += 1
            continue
        payload_off = off + hdr_len
        end = payload_off + payload_len
        common = {
            "offset": off,
            "record_type": TYPE_NAMES.get(record_type, f"0x{record_type:x}"),
            "record_type_raw": f"0x{record_type:08x}",
            "version": version,
            "known_version": version in (1, 2),
            "compatible_version": version in (1, 2),
            "forward_skippable": version > 2,
            "hdr_len": hdr_len,
            "flags": flags,
            "seq": seq,
            "payload_len": payload_len,
            "meta0": meta0,
            "meta1": meta1,
            "timestamp_ns": timestamp_ns,
            "timestamp_clock": "monotonic" if version >= 2 else "unspecified",
            "header_extension_len": hdr_len - minimum_hdr_len,
        }
        if off + minimum_hdr_len > len(buf):
            rec = dict(common)
            rec.update({
                "record_type": "truncated_header",
                "type": "truncated_header",
                "declared_header_len": hdr_len,
                "available": len(buf) - off,
            })
            yield rec, b""
            break
        if version >= 2:
            session_id, pdev_id, reserved0, reserved1 = struct.unpack_from(
                "<QIIQ", buf, off + CFRR_V1_HDR.size
            )
            common.update({
                "session_id": session_id,
                "pdev_id": pdev_id,
                "header_reserved0": reserved0,
                "header_reserved1": reserved1,
                "header_reserved_zero": reserved0 == 0 and reserved1 == 0,
            })
        if payload_off > len(buf):
            rec = dict(common)
            rec.update({
                "record_type": "truncated_header",
                "type": "truncated_header",
                "declared_header_len": hdr_len,
                "available": len(buf) - off,
            })
            yield rec, b""
            break
        if end > len(buf):
            rec = dict(common)
            rec.update({
                "record_type": "truncated_frame",
                "type": "truncated_frame",
                "declared_payload_len": payload_len,
                "available": max(0, len(buf) - payload_off),
            })
            yield rec, buf[payload_off:]
            break

        payload = buf[payload_off:end]
        rec: dict[str, Any] = common
        if version in (1, 2):
            rec.update(summarize_payload(record_type, payload))
        else:
            rec["opaque_payload_sha256"] = sha256_hex(payload)
        yield rec, payload
        off = end


def parse_framed(buf: bytes) -> list[dict[str, Any]]:
    return [rec for rec, _payload in iter_framed_payloads(buf)]


def parse_unframed(buf: bytes) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    final_ranges: list[tuple[int, int]] = []

    start = 0
    while True:
        off = buf.find(CSI_START_BYTES, start)
        if off < 0:
            break
        csi = parse_csi_common(buf, off)
        if csi:
            end_off = csi.get("end_magic_offset")
            if isinstance(end_off, int):
                final_ranges.append((off, off + end_off + 4))
        start = off + 1

    for off in range(0, len(buf) - RAW_ENH_HDR_LEN, 2):
        if any(start < off < end for start, end in final_ranges):
            continue
        raw = parse_raw_dbr_payload(buf, off)
        if raw_dbr_is_sane(raw, len(buf) - off) and buf[off:off + 2] == b"\xba\x0c":
            records.append({
                "offset": off,
                "record_type": "raw_dbr",
                "payload_len": raw["parsed_len"],
                "raw_dbr": raw,
            })

    for marker, name, parser, size in (
        (struct.pack("<I", RX_PPDU_MAGIC), "rx_ppdu", parse_rx_ppdu_snapshot, 40),
        (CSI_START_BYTES, "final", parse_csi_common, None),
    ):
        start = 0
        while True:
            off = buf.find(marker, start)
            if off < 0:
                break
            payload = buf[off:] if size is None else buf[off:off + size]
            records.append({
                "offset": off,
                "record_type": name,
                **summarize_payload(TYPE_RX_PPDU if name == "rx_ppdu" else TYPE_FINAL, payload),
            })
            start = off + 1
    records.sort(key=lambda item: item["offset"])
    return records


def payload_for_unframed_record(buf: bytes, rec: dict[str, Any]) -> bytes:
    off = rec["offset"]
    if rec["record_type"] == "raw_dbr" and rec.get("raw_dbr"):
        return buf[off:off + rec["raw_dbr"]["parsed_len"]]
    if rec["record_type"] == "rx_ppdu":
        return buf[off:off + 40]
    if rec["record_type"] == "final" and rec.get("csi_cfr"):
        end_off = rec["csi_cfr"].get("end_magic_offset")
        if isinstance(end_off, int):
            return buf[off:off + end_off + 4]
    return b""


def load_capture_metadata(capture: Path, explicit: Path | None) -> dict[str, Any] | None:
    candidates = []
    if explicit:
        candidates.append(explicit)
    else:
        candidates.append(Path(str(capture) + ".json"))
        rotated = re.match(r"^(.*)\.\d{6}\.cfrr$", str(capture))
        if rotated:
            candidates.append(Path(rotated.group(1) + ".json"))
    for path in candidates:
        if path.exists():
            with path.open("r", encoding="utf-8") as f:
                data = json.load(f)
            data["metadata_file"] = str(path)
            return data
    return None


def validate_capture_metadata(
    capture: Path, buf: bytes, metadata: dict[str, Any] | None
) -> dict[str, Any] | None:
    if not metadata:
        return None
    mismatches: list[str] = []
    segments = metadata.get("segments")
    segment = None
    if isinstance(segments, list):
        capture_abs = capture.resolve()
        basename_matches = []
        for candidate in segments:
            if not isinstance(candidate, dict):
                continue
            path = candidate.get("path")
            if isinstance(path, str):
                if Path(path).resolve() == capture_abs:
                    segment = candidate
                    break
                if Path(path).name == capture.name:
                    basename_matches.append(candidate)
        if segment is None and len(basename_matches) == 1:
            segment = basename_matches[0]
        if segment is None:
            mismatches.append("capture_not_in_segment_manifest")
    if isinstance(segment, dict):
        expected_bytes = segment.get("bytes")
        if isinstance(expected_bytes, int) and expected_bytes != len(buf):
            mismatches.append("segment_size_mismatch")
        expected_sha256 = segment.get("sha256")
        actual_sha256 = sha256_hex(buf)
        if isinstance(expected_sha256, str) and expected_sha256 != actual_sha256:
            mismatches.append("segment_sha256_mismatch")
        if segment.get("complete") is False:
            mismatches.append("segment_marked_incomplete")
    else:
        expected_bytes = metadata.get("output_bytes")
        if isinstance(expected_bytes, int) and expected_bytes != len(buf):
            mismatches.append("capture_size_mismatch")
        actual_sha256 = sha256_hex(buf)
    if metadata.get("exit_status") not in (None, 0):
        mismatches.append("recorder_exit_nonzero")
    return {
        "status": "ok" if not mismatches else "mismatch",
        "mismatches": mismatches,
        "actual_size": len(buf),
        "actual_sha256": actual_sha256,
    }


def annotate_stale(records: list[dict[str, Any]], metadata: dict[str, Any] | None) -> None:
    if not metadata:
        return
    start_ns = metadata.get("start_real_ns")
    end_ns = metadata.get("end_real_ns")
    if not isinstance(start_ns, int) or not isinstance(end_ns, int):
        return
    for rec in records:
        if rec.get("timestamp_clock") == "monotonic":
            rec["metadata_window"] = "not_comparable_monotonic"
            rec["stale"] = False
            continue
        ts = rec.get("timestamp_ns")
        if not isinstance(ts, int):
            continue
        if ts < start_ns:
            rec["metadata_window"] = "before_start"
            rec["stale"] = True
        elif ts > end_ns:
            rec["metadata_window"] = "after_end"
            rec["stale"] = True
        else:
            rec["metadata_window"] = "inside"
            rec["stale"] = False


def raw_payload_from_record(record: dict[str, Any], payload: bytes) -> tuple[bytes, dict[str, Any], str] | None:
    if record.get("record_type") == "raw_dbr" and record.get("raw_dbr"):
        raw = record["raw_dbr"]
        valid_len = raw.get("parsed_len")
        if not isinstance(valid_len, int) or valid_len <= 0:
            return None
        return payload[:valid_len], raw, "raw_dbr"

    if record.get("record_type") == "final" and record.get("csi_cfr"):
        csi = record["csi_cfr"]
        raw = csi.get("raw_payload_header")
        if not raw:
            return None
        header_len = csi.get("header_len")
        valid_len = raw.get("parsed_len")
        if not isinstance(header_len, int) or not isinstance(valid_len, int):
            return None
        return payload[header_len:header_len + valid_len], raw, "final"

    return None


def raw_payload_fingerprint(record: dict[str, Any], payload: bytes) -> str | None:
    if record.get("record_type") == "final":
        csi = record.get("csi_cfr")
        if not isinstance(csi, dict) or not csi.get("end_magic_ok_at_expected_offset"):
            return None
    raw_tuple = raw_payload_from_record(record, payload)
    if raw_tuple is None:
        return None
    raw_payload, raw_header, _source = raw_tuple
    valid_len = raw_header.get("parsed_len")
    if not isinstance(valid_len, int) or valid_len <= 0 or len(raw_payload) < valid_len:
        return None
    if not raw_dbr_is_sane(raw_header, len(raw_payload)):
        return None
    return sha256_hex(raw_payload[:valid_len])


def raw_payload_match_key(
    record: dict[str, Any], payload: bytes
) -> tuple[int | None, str] | None:
    fingerprint = raw_payload_fingerprint(record, payload)
    if fingerprint is None:
        return None
    session_id = record.get("session_id")
    return (session_id if isinstance(session_id, int) else None, fingerprint)


def unwrap_phases(phases: list[float]) -> list[float]:
    if not phases:
        return []
    out = [phases[0]]
    offset = 0.0
    prev = phases[0]
    for phase in phases[1:]:
        delta = phase - prev
        if delta > math.pi:
            offset -= 2.0 * math.pi
        elif delta < -math.pi:
            offset += 2.0 * math.pi
        out.append(phase + offset)
        prev = phase
    return out


def sample_position(index: int, header: dict[str, Any], lane_order: str) -> tuple[int, int, int, int]:
    lane_count = int(header.get("lane_count") or 1)
    chain_count = int(header.get("chain_count") or 1)
    stream_count = int(header.get("stream_count") or 1)
    tone_count = header.get("tone_count_per_lane")
    if lane_order == "flat" or lane_count <= 1 or not isinstance(tone_count, int) or tone_count <= 0:
        lane = 0
        tone = index
    elif lane_order == "contiguous":
        lane = index // tone_count
        tone = index % tone_count
    else:  # interleaved
        lane = index % lane_count
        tone = index // lane_count

    if lane_order == "flat":
        chain = 0
        stream = 0
    else:
        chain = lane % max(chain_count, 1)
        stream = (lane // max(chain_count, 1)) % max(stream_count, 1)
    return lane, chain, stream, tone


def decode_cfr_samples(
    raw_payload: bytes,
    raw_header: dict[str, Any],
    *,
    iq_layout: str = "iq16-le",
    lane_order: str = "interleaved",
) -> dict[str, Any]:
    warnings = [
        "I/Q order and lane/subcarrier order are inferred; the local kernel tree does not document them.",
    ]
    if raw_header.get("cfr_fmt") != 0:
        raise DecodeError(
            f"cfr_fmt={raw_header.get('cfr_fmt')} is 24-bit compressed/undocumented in this tree"
        )
    if raw_header.get("sample_remainder_bytes"):
        raise DecodeError("sample byte count is not divisible by the complex sample size")

    sample_offset = raw_header["sample_offset"]
    sample_len = raw_header["sample_bytes"]
    sample_end = sample_offset + sample_len
    if sample_end > len(raw_payload):
        raise DecodeError(
            f"sample region overruns raw payload: end={sample_end} len={len(raw_payload)}"
        )
    sample_bytes = raw_payload[sample_offset:sample_end]

    samples: list[dict[str, Any]] = []
    for idx in range(0, len(sample_bytes), 4):
        first, second = struct.unpack_from("<hh", sample_bytes, idx)
        if iq_layout == "qi16-le":
            i_val, q_val = second, first
        else:
            i_val, q_val = first, second
        amp = math.hypot(i_val, q_val)
        phase = math.atan2(q_val, i_val)
        sample_index = idx // 4
        lane, chain, stream, tone = sample_position(sample_index, raw_header, lane_order)
        samples.append({
            "sample_index": sample_index,
            "tone_index": tone,
            "lane_index": lane,
            "chain_index": chain,
            "stream_index": stream,
            "i": i_val,
            "q": q_val,
            "amplitude": amp,
            "phase_rad": phase,
        })

    for lane in sorted({sample["lane_index"] for sample in samples}):
        lane_samples = sorted(
            (sample for sample in samples if sample["lane_index"] == lane),
            key=lambda sample: sample["tone_index"],
        )
        unwrapped = unwrap_phases([sample["phase_rad"] for sample in lane_samples])
        for sample, phase_unwrapped in zip(lane_samples, unwrapped):
            sample["phase_unwrapped_rad"] = phase_unwrapped

    amps = [sample["amplitude"] for sample in samples]
    phases = [sample["phase_rad"] for sample in samples]
    summary = {
        "decode_status": "ok",
        "iq_layout": iq_layout,
        "lane_order": lane_order,
        "sample_count": len(samples),
        "lane_count": raw_header.get("lane_count"),
        "chain_count": raw_header.get("chain_count"),
        "stream_count": raw_header.get("stream_count"),
        "tone_count_per_lane": raw_header.get("tone_count_per_lane"),
        "sample_offset": sample_offset,
        "sample_bytes": sample_len,
        "amplitude_min": min(amps) if amps else None,
        "amplitude_max": max(amps) if amps else None,
        "amplitude_mean": (sum(amps) / len(amps)) if amps else None,
        "phase_min_rad": min(phases) if phases else None,
        "phase_max_rad": max(phases) if phases else None,
        "warnings": warnings,
    }
    return {"summary": summary, "samples": samples}


def lane_series(samples: list[dict[str, Any]], key: str) -> dict[int, list[tuple[int, float]]]:
    series: dict[int, list[tuple[int, float]]] = {}
    for sample in samples:
        value = sample.get(key)
        if value is None:
            continue
        lane = int(sample["lane_index"])
        series.setdefault(lane, []).append((int(sample["tone_index"]), float(value)))
    for values in series.values():
        values.sort(key=lambda item: item[0])
    return series


def write_samples_csv(path: Path, samples: list[dict[str, Any]]) -> None:
    fieldnames = [
        "sample_index",
        "tone_index",
        "lane_index",
        "chain_index",
        "stream_index",
        "i",
        "q",
        "amplitude",
        "phase_rad",
        "phase_unwrapped_rad",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for sample in samples:
            row = dict(sample)
            for key in ("amplitude", "phase_rad", "phase_unwrapped_rad"):
                if key in row and row[key] is not None:
                    row[key] = f"{float(row[key]):.9g}"
            writer.writerow({key: row.get(key) for key in fieldnames})


def write_svg_plot(
    path: Path,
    series: dict[int, list[tuple[int, float]]],
    *,
    title: str,
    ylabel: str,
    force_y: tuple[float, float] | None = None,
) -> None:
    width = 1200
    height = 520
    left = 76
    right = 24
    top = 44
    bottom = 56
    plot_w = width - left - right
    plot_h = height - top - bottom

    all_points = [point for values in series.values() for point in values]
    if not all_points:
        path.write_text("<svg xmlns='http://www.w3.org/2000/svg'></svg>\n", encoding="utf-8")
        return

    x_min = min(x for x, _y in all_points)
    x_max = max(x for x, _y in all_points)
    y_min = min(y for _x, y in all_points)
    y_max = max(y for _x, y in all_points)
    if force_y:
        y_min, y_max = force_y
    if x_min == x_max:
        x_max = x_min + 1
    if y_min == y_max:
        y_min -= 1.0
        y_max += 1.0

    def sx(x: float) -> float:
        return left + ((x - x_min) / (x_max - x_min)) * plot_w

    def sy(y: float) -> float:
        return top + (1.0 - ((y - y_min) / (y_max - y_min))) * plot_h

    lines = [
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
        f"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{width}\" height=\"{height}\" viewBox=\"0 0 {width} {height}\">",
        "<style>text{font-family:monospace;font-size:13px}.axis{stroke:#222;stroke-width:1}.grid{stroke:#ddd;stroke-width:1}.legend{font-size:12px}</style>",
        f"<rect x=\"0\" y=\"0\" width=\"{width}\" height=\"{height}\" fill=\"white\"/>",
        f"<text x=\"{left}\" y=\"24\">{html.escape(title)}</text>",
        f"<line class=\"axis\" x1=\"{left}\" y1=\"{top + plot_h}\" x2=\"{left + plot_w}\" y2=\"{top + plot_h}\"/>",
        f"<line class=\"axis\" x1=\"{left}\" y1=\"{top}\" x2=\"{left}\" y2=\"{top + plot_h}\"/>",
    ]

    for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
        x = left + frac * plot_w
        y = top + frac * plot_h
        xv = x_min + frac * (x_max - x_min)
        yv = y_max - frac * (y_max - y_min)
        lines.append(f"<line class=\"grid\" x1=\"{x:.1f}\" y1=\"{top}\" x2=\"{x:.1f}\" y2=\"{top + plot_h}\"/>")
        lines.append(f"<line class=\"grid\" x1=\"{left}\" y1=\"{y:.1f}\" x2=\"{left + plot_w}\" y2=\"{y:.1f}\"/>")
        lines.append(f"<text x=\"{x - 18:.1f}\" y=\"{top + plot_h + 22}\">{xv:.0f}</text>")
        lines.append(f"<text x=\"4\" y=\"{y + 4:.1f}\">{yv:.3g}</text>")

    lines.append(f"<text x=\"{left + plot_w / 2 - 34:.1f}\" y=\"{height - 12}\">tone index</text>")
    lines.append(f"<text x=\"8\" y=\"{top - 12}\">{html.escape(ylabel)}</text>")

    for lane, values in sorted(series.items()):
        color = SVG_COLORS[lane % len(SVG_COLORS)]
        points = " ".join(f"{sx(x):.2f},{sy(y):.2f}" for x, y in values)
        lines.append(f"<polyline fill=\"none\" stroke=\"{color}\" stroke-width=\"1.4\" points=\"{points}\"/>")
        legend_x = left + 12 + (lane % 4) * 160
        legend_y = top + 18 + (lane // 4) * 18
        lines.append(f"<rect x=\"{legend_x}\" y=\"{legend_y - 10}\" width=\"10\" height=\"10\" fill=\"{color}\"/>")
        lines.append(f"<text class=\"legend\" x=\"{legend_x + 16}\" y=\"{legend_y}\">lane {lane}</text>")

    lines.append("</svg>")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def safe_stem(record: dict[str, Any], source: str, index: int, raw_header: dict[str, Any]) -> str:
    seq = record.get("seq")
    ppdu = raw_header.get("phy_ppdu_id")
    parts = [f"record_{index:04d}", source]
    if isinstance(seq, int):
        parts.append(f"seq{seq:06d}")
    if isinstance(ppdu, int):
        parts.append(f"ppdu{ppdu:05d}")
    return "_".join(parts)


def select_export_records(
    records_with_payload: list[tuple[dict[str, Any], bytes]],
    source: str,
) -> list[tuple[dict[str, Any], bytes]]:
    if source == "all":
        names = {"raw_dbr", "final"}
    elif source == "raw_dbr":
        names = {"raw_dbr"}
    elif source == "final":
        names = {"final"}
    else:
        final_fingerprints = Counter(
            key
            for rec, payload in records_with_payload
            if rec.get("record_type") == "final"
            if (key := raw_payload_match_key(rec, payload)) is not None
        )
        selected: list[tuple[dict[str, Any], bytes]] = []
        for rec, payload in records_with_payload:
            name = rec.get("record_type")
            key = raw_payload_match_key(rec, payload)
            if name == "final" and key is not None:
                selected.append((rec, payload))
            elif name == "raw_dbr" and key is not None:
                if final_fingerprints[key]:
                    final_fingerprints[key] -= 1
                else:
                    selected.append((rec, payload))
        return selected
    return [
        (rec, payload) for rec, payload in records_with_payload
        if rec.get("record_type") in names
    ]


def export_decoded_csi(
    records_with_payload: list[tuple[dict[str, Any], bytes]],
    out_dir: Path,
    *,
    source: str,
    iq_layout: str,
    lane_order: str,
    max_records: int,
    write_csv_files: bool,
    write_plot_files: bool,
    drop_stale: bool,
) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    selected = select_export_records(records_with_payload, source)
    exports: list[dict[str, Any]] = []
    attempted = 0

    records_jsonl = out_dir / "records.jsonl"
    with records_jsonl.open("w", encoding="utf-8") as jsonl:
        for rec, payload in selected:
            if drop_stale and rec.get("stale"):
                continue
            if max_records and attempted >= max_records:
                break
            raw_tuple = raw_payload_from_record(rec, payload)
            if raw_tuple is None:
                continue
            raw_payload, raw_header, payload_source = raw_tuple
            attempted += 1
            export_item: dict[str, Any] = {
                "record_index": attempted,
                "source": payload_source,
                "record_type": rec.get("record_type"),
                "seq": rec.get("seq"),
                "timestamp_ns": rec.get("timestamp_ns"),
                "stale": rec.get("stale"),
                "metadata_window": rec.get("metadata_window"),
                "raw_header": raw_header,
            }
            try:
                decoded = decode_cfr_samples(
                    raw_payload,
                    raw_header,
                    iq_layout=iq_layout,
                    lane_order=lane_order,
                )
                samples = decoded["samples"]
                summary = decoded["summary"]
                export_item["decode"] = summary
                stem = safe_stem(rec, payload_source, attempted, raw_header)

                if write_csv_files:
                    csv_path = out_dir / f"{stem}.csv"
                    write_samples_csv(csv_path, samples)
                    export_item["csv"] = str(csv_path)

                if write_plot_files:
                    amp_path = out_dir / f"{stem}_amplitude.svg"
                    phase_path = out_dir / f"{stem}_phase.svg"
                    unwrapped_path = out_dir / f"{stem}_phase_unwrapped.svg"
                    write_svg_plot(
                        amp_path,
                        lane_series(samples, "amplitude"),
                        title=f"CFR amplitude {stem}",
                        ylabel="amplitude",
                    )
                    write_svg_plot(
                        phase_path,
                        lane_series(samples, "phase_rad"),
                        title=f"CFR phase {stem}",
                        ylabel="phase rad",
                        force_y=(-math.pi, math.pi),
                    )
                    write_svg_plot(
                        unwrapped_path,
                        lane_series(samples, "phase_unwrapped_rad"),
                        title=f"CFR unwrapped phase {stem}",
                        ylabel="phase rad unwrapped",
                    )
                    export_item["plots"] = {
                        "amplitude_svg": str(amp_path),
                        "phase_svg": str(phase_path),
                        "phase_unwrapped_svg": str(unwrapped_path),
                    }
            except DecodeError as exc:
                export_item["decode"] = {
                    "decode_status": "unsupported",
                    "error": str(exc),
                    "iq_layout": iq_layout,
                    "lane_order": lane_order,
                }
            jsonl.write(json.dumps(export_item, sort_keys=True) + "\n")
            exports.append(export_item)

    summary = {
        "export_dir": str(out_dir),
        "source_policy": source,
        "iq_layout": iq_layout,
        "lane_order": lane_order,
        "max_records": max_records,
        "records_jsonl": str(records_jsonl),
        "decoded_records": sum(1 for item in exports if item.get("decode", {}).get("decode_status") == "ok"),
        "unsupported_records": sum(1 for item in exports if item.get("decode", {}).get("decode_status") == "unsupported"),
        "records": exports,
    }
    summary_path = out_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    summary["summary_json"] = str(summary_path)
    return summary


def add_decode_summaries(
    records_with_payload: list[tuple[dict[str, Any], bytes]],
    *,
    iq_layout: str,
    lane_order: str,
) -> None:
    for rec, payload in records_with_payload:
        raw_tuple = raw_payload_from_record(rec, payload)
        if raw_tuple is None:
            continue
        raw_payload, raw_header, payload_source = raw_tuple
        try:
            decoded = decode_cfr_samples(
                raw_payload,
                raw_header,
                iq_layout=iq_layout,
                lane_order=lane_order,
            )
            rec["csi_decode"] = decoded["summary"] | {"source": payload_source}
        except DecodeError as exc:
            rec["csi_decode"] = {
                "decode_status": "unsupported",
                "error": str(exc),
                "source": payload_source,
                "iq_layout": iq_layout,
                "lane_order": lane_order,
            }


def analyze_framing(records: list[dict[str, Any]]) -> dict[str, Any]:
    versions: Counter[int] = Counter()
    session_ids: set[int] = set()
    sequence_gaps = 0
    sequence_resets = 0
    out_of_order_records = 0
    session_starts = 0
    session_ends = 0
    invalid_session_records = 0
    rearm_records = 0
    soft_rearms = 0
    hard_rearms = 0
    blind_rearms = 0
    rearm_failures = 0
    nonzero_reserved_headers = 0
    truncated_records = 0
    kernel_reported_drops = 0
    last_key: tuple[str, int] | None = None
    last_seq: int | None = None
    v1_epoch = 0
    saw_v1_record = False

    for rec in records:
        version = rec.get("version")
        if isinstance(version, int):
            versions[version] += 1
        if rec.get("record_type") in ("truncated_header", "truncated_frame"):
            truncated_records += 1
        if rec.get("header_reserved_zero") is False:
            nonzero_reserved_headers += 1

        record_type = rec.get("record_type")
        boundary = False
        if record_type == "session_start":
            session = rec.get("session_start")
            if isinstance(session, dict) and session.get("valid"):
                session_starts += 1
                boundary = True
                if version == 1:
                    v1_epoch += 1
            else:
                invalid_session_records += 1
        elif record_type == "session_end":
            session = rec.get("session_end")
            if isinstance(session, dict) and session.get("valid"):
                session_ends += 1
                dropped = session.get("records_dropped")
                if isinstance(dropped, int):
                    kernel_reported_drops += dropped
            else:
                invalid_session_records += 1
        elif record_type == "rearm":
            rearm = rec.get("rearm")
            if isinstance(rearm, dict) and rearm.get("valid"):
                rearm_records += 1
                soft_rearms += rearm.get("stage") == 1
                hard_rearms += rearm.get("stage") == 2
                blind_rearms += rearm.get("stage") == 3
                rearm_failures += rearm.get("status") != 0

        if isinstance(version, int) and version >= 2:
            session_id = rec.get("session_id")
            if not isinstance(session_id, int):
                continue
            session_ids.add(session_id)
            key = ("v2", session_id)
        else:
            saw_v1_record = True
            key = ("v1", v1_epoch)

        seq = rec.get("seq")
        if not isinstance(seq, int):
            continue
        if boundary or key != last_key or last_seq is None:
            last_key = key
            last_seq = seq
            continue
        expected = (last_seq + 1) & 0xFFFFFFFF
        delta = (seq - expected) & 0xFFFFFFFF
        if delta:
            if delta < 0x80000000:
                sequence_gaps += delta
            else:
                sequence_resets += 1
                out_of_order_records += 1
                continue
        last_seq = seq

    inferred_v1_sessions = 0
    if saw_v1_record:
        inferred_v1_sessions = v1_epoch or 1
    return {
        "versions": {str(version): count for version, count in sorted(versions.items())},
        "unknown_compatible_versions": sum(
            count for version, count in versions.items() if version > 2
        ),
        "sessions": len(session_ids) + inferred_v1_sessions,
        "session_ids": sorted(session_ids),
        "session_starts": session_starts,
        "session_ends": session_ends,
        "invalid_session_records": invalid_session_records,
        "rearm_records": rearm_records,
        "soft_rearms": soft_rearms,
        "hard_rearms": hard_rearms,
        "blind_rearms": blind_rearms,
        "rearm_failures": rearm_failures,
        "sequence_gaps": sequence_gaps,
        "sequence_resets": sequence_resets,
        "out_of_order_records": out_of_order_records,
        "nonzero_reserved_headers": nonzero_reserved_headers,
        "truncated_records": truncated_records,
        "kernel_reported_drops": kernel_reported_drops,
    }


def build_result(
    capture: Path,
    buf: bytes,
    records: list[dict[str, Any]],
    mode: str,
    metadata: dict[str, Any] | None,
) -> dict[str, Any]:
    counts: dict[str, int] = {}
    for rec in records:
        name = str(rec.get("record_type"))
        counts[name] = counts.get(name, 0) + 1
    stale_count = sum(1 for rec in records if rec.get("stale"))
    return {
        "file": str(capture),
        "size": len(buf),
        "mode": mode,
        "record_count": len(records),
        "type_counts": counts,
        "framing": analyze_framing(records),
        "metadata": metadata,
        "metadata_validation": validate_capture_metadata(capture, buf, metadata),
        "stale_record_count": stale_count,
        "records": records,
    }


def print_text_summary(result: dict[str, Any]) -> None:
    print(f"file: {result['file']}")
    print(f"size: {result['size']} bytes")
    print(f"mode: {result['mode']}")
    print(f"records: {result['record_count']}")
    framing = result.get("framing", {})
    print(
        f"sessions: {framing.get('sessions', 0)}"
        f" sequence_gaps: {framing.get('sequence_gaps', 0)}"
        f" sequence_resets: {framing.get('sequence_resets', 0)}"
    )
    if result.get("metadata"):
        print(f"metadata: {result['metadata'].get('metadata_file')}")
        print(f"stale_records: {result.get('stale_record_count', 0)}")
    for rec in result["records"]:
        line = f"offset={rec['offset']} type={rec['record_type']}"
        if "seq" in rec:
            line += f" seq={rec['seq']}"
        if "session_id" in rec:
            line += f" session={rec['session_id']}"
        if "payload_len" in rec:
            line += f" payload_len={rec['payload_len']}"
        if rec.get("stale"):
            line += f" stale={rec.get('metadata_window')}"
        if "raw_dbr" in rec and rec["raw_dbr"]:
            raw = rec["raw_dbr"]
            line += (
                f" ppdu={raw['phy_ppdu_id']} bw={raw['bw']}({raw.get('bandwidth_mhz')}MHz)"
                f" chains={raw['chains']}({raw.get('chain_count')})"
                f" nss={raw['nss']}({raw.get('stream_count')})"
                f" raw_len={raw['parsed_len']} sample_off={raw['sample_offset']}"
                f" tones={raw.get('tone_count_per_lane')}"
            )
        if "rx_ppdu" in rec and rec["rx_ppdu"]:
            line += f" ppdu={rec['rx_ppdu']['ppdu_id']} addr={rec['rx_ppdu'].get('rtt_buffer_addr_40')}"
        if "dbr_meta" in rec and rec["dbr_meta"]:
            meta = rec["dbr_meta"]
            line += (
                f" cookie={meta['cookie']} ppdu={meta['phy_ppdu_id']}"
                f" bw={meta['upload_pkt_bw']}({meta.get('bandwidth_mhz')}MHz)"
                f" chains={meta['num_chains']}({meta.get('chain_count')})"
                f" nss={meta['nss']}({meta.get('stream_count')})"
                f" parsed_len={meta['parsed_len']} sample_off={meta['sample_offset']}"
                f" tones={meta.get('tone_count_per_lane')}"
            )
        if "csi_cfr" in rec and rec["csi_cfr"]:
            csi = rec["csi_cfr"]
            line += (
                f" vendor={csi['vendor']} meta_v={csi['metadata_version']}"
                f" chip={csi['chip_type']}"
            )
            raw = csi.get("raw_payload_header")
            if raw:
                line += (
                    f" ppdu={raw['phy_ppdu_id']} raw_len={raw['parsed_len']}"
                    f" sample_off={raw['sample_offset']} tones={raw.get('tone_count_per_lane')}"
                )
        if "csi_decode" in rec:
            dec = rec["csi_decode"]
            line += f" decode={dec.get('decode_status')} samples={dec.get('sample_count')}"
        if "session_start" in rec:
            session = rec["session_start"]
            line += (
                f" valid={session.get('valid')} pdev={session.get('pdev_id')}"
                f" framing_v={session.get('framing_version')}"
                f" relay={session.get('relay_subbuf_size')}x"
                f"{session.get('relay_num_subbufs')}"
                f" count={session.get('capture_count')}"
                f" interval_mode={session.get('capture_interval_mode')}"
                f" continuous={session.get('continuous_enabled')}"
            )
        if "session_end" in rec:
            session = rec["session_end"]
            line += (
                f" valid={session.get('valid')} stop={session.get('stop_reason_name')}"
                f" committed={session.get('records_committed')}"
                f" dropped={session.get('records_dropped')}"
            )
        if "rearm" in rec:
            rearm = rec["rearm"]
            line += (
                f" valid={rearm.get('valid')} epoch={rearm.get('epoch')}"
                f" stage={rearm.get('stage_name')} status={rearm.get('status')}"
            )
        print(line)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", type=Path, help="CFR relay capture file")
    ap.add_argument("--json", action="store_true", help="emit JSON summary")
    ap.add_argument("--metadata", type=Path, help="recorder metadata JSON; defaults to CAPTURE.json if present")
    ap.add_argument("--drop-stale", action="store_true", help="with --export-dir, skip records outside metadata start/end window")
    ap.add_argument("--decode-csi", action="store_true", help="add decoded CSI summary fields to text/JSON output")
    ap.add_argument("--export-dir", type=Path, help="write decoded CSI CSV/JSONL/SVG artifacts here")
    ap.add_argument(
        "--source",
        choices=("auto", "final", "raw_dbr", "all"),
        default="auto",
        help="records to decode/export; auto keeps valid finals and unmatched raw DBRs",
    )
    ap.add_argument(
        "--iq-layout",
        choices=("iq16-le", "qi16-le"),
        default="iq16-le",
        help="candidate 32-bit raw tone layout; source tree does not document I/Q order",
    )
    ap.add_argument(
        "--lane-order",
        choices=("interleaved", "contiguous", "flat"),
        default="interleaved",
        help="candidate matrix ordering for lane/tone plots",
    )
    ap.add_argument(
        "--max-export-records",
        type=int,
        default=0,
        help="limit decoded exports; 0 means all selected records",
    )
    ap.add_argument("--no-csv", action="store_true", help="do not write per-record CSV sample files")
    ap.add_argument("--no-plots", action="store_true", help="do not write SVG amplitude/phase plots")
    args = ap.parse_args()

    buf = args.capture.read_bytes()
    framed_payloads = list(iter_framed_payloads(buf))
    if framed_payloads:
        records_with_payload = framed_payloads
        records = [rec for rec, _payload in records_with_payload]
        mode = "framed"
    else:
        records = parse_unframed(buf)
        records_with_payload = [(rec, payload_for_unframed_record(buf, rec)) for rec in records]
        mode = "unframed-scan"

    metadata = load_capture_metadata(args.capture, args.metadata)
    annotate_stale(records, metadata)

    if args.decode_csi:
        add_decode_summaries(
            records_with_payload,
            iq_layout=args.iq_layout,
            lane_order=args.lane_order,
        )

    result = build_result(args.capture, buf, records, mode, metadata)

    if args.export_dir:
        export = export_decoded_csi(
            records_with_payload,
            args.export_dir,
            source=args.source,
            iq_layout=args.iq_layout,
            lane_order=args.lane_order,
            max_records=args.max_export_records,
            write_csv_files=not args.no_csv,
            write_plot_files=not args.no_plots,
            drop_stale=args.drop_stale,
        )
        result["decode_export"] = export

    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0

    print_text_summary(result)
    if result.get("decode_export"):
        export = result["decode_export"]
        print(f"decode_export_dir: {export['export_dir']}")
        print(f"decode_summary_json: {export['summary_json']}")
        print(f"decoded_records: {export['decoded_records']}")
        print(f"unsupported_records: {export['unsupported_records']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
