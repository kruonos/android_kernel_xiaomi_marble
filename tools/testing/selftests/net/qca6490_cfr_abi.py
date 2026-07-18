#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check CFRR wire declarations, conversion sites, and golden bytes."""

import argparse
import re
import struct
import sys
from pathlib import Path


TYPE_FORMAT = {
    "uint8_t": "B",
    "uint16_t": "H",
    "uint32_t": "I",
    "uint64_t": "Q",
    "__le16": "H",
    "__le32": "I",
    "__le64": "Q",
}

HEADER_FIELDS = [
    ("__le32", "magic"),
    ("__le16", "version"),
    ("__le16", "hdr_len"),
    ("__le32", "type"),
    ("__le32", "flags"),
    ("__le32", "seq"),
    ("__le32", "payload_len"),
    ("__le32", "meta0"),
    ("__le32", "meta1"),
    ("__le64", "timestamp_ns"),
    ("__le64", "session_id"),
    ("__le32", "pdev_id"),
    ("__le32", "reserved0"),
    ("__le64", "reserved1"),
]

DBR_META_FIELDS = [
    ("uint32_t", "magic"),
    ("uint16_t", "version"),
    ("uint16_t", "meta_len"),
    ("uint32_t", "flags"),
    ("uint32_t", "cookie"),
    ("uint32_t", "phy_ppdu_id"),
    ("uint32_t", "paddr_low32"),
    ("uint32_t", "paddr_high32"),
    ("uint32_t", "dbr_len"),
    ("uint32_t", "parsed_len"),
    ("uint16_t", "dma_hdr_bytes"),
    ("uint16_t", "dma_hdr_words"),
    ("uint16_t", "freeze_tlv_len"),
    ("uint16_t", "mu_rx_user_size"),
    ("uint16_t", "mu_rx_num_users"),
    ("uint16_t", "sample_offset"),
    ("uint32_t", "sample_len"),
    ("uint8_t", "tag"),
    ("uint8_t", "upload_done"),
    ("uint8_t", "capture_type"),
    ("uint8_t", "preamble_type"),
    ("uint8_t", "nss"),
    ("uint8_t", "num_chains"),
    ("uint8_t", "upload_pkt_bw"),
    ("uint8_t", "sw_peer_id_valid"),
    ("uint16_t", "sw_peer_id"),
    ("uint16_t", "total_bytes"),
    ("uint8_t", "header_version"),
    ("uint8_t", "target_id"),
    ("uint8_t", "cfr_fmt"),
    ("uint8_t", "mu_rx_data_incl"),
    ("uint8_t", "freeze_data_incl"),
    ("uint8_t", "freeze_tlv_version"),
    ("uint8_t", "decimation_factor"),
    ("uint8_t", "reserved0"),
    ("uint8_t", "freeze"),
    ("uint8_t", "freeze_capture_reason"),
    ("uint8_t", "freeze_packet_type"),
    ("uint8_t", "freeze_packet_subtype"),
    ("uint8_t", "freeze_directed"),
    ("uint8_t", "freeze_sw_peer_id_valid"),
    ("uint16_t", "freeze_sw_peer_id"),
    ("uint16_t", "freeze_phy_ppdu_id"),
    ("uint16_t", "packet_ta_lower_16"),
    ("uint16_t", "packet_ta_mid_16"),
    ("uint16_t", "packet_ta_upper_16"),
    ("uint16_t", "packet_ra_lower_16"),
    ("uint16_t", "packet_ra_mid_16"),
    ("uint16_t", "packet_ra_upper_16"),
    ("uint16_t", "tsf_word0"),
    ("uint16_t", "tsf_word1"),
    ("uint16_t", "tsf_word2"),
    ("uint16_t", "tsf_word3_or_user_mask_36_32"),
    ("uint16_t", "user_mask_word0"),
    ("uint16_t", "user_mask_word1"),
    ("uint16_t", "user_mask_word2"),
    ("uint32_t", "raw_header_len"),
]

RX_PPDU_FIELDS = [
    ("uint32_t", "magic"),
    ("uint32_t", "ppdu_id"),
    ("uint32_t", "bb_captured_channel"),
    ("uint32_t", "rx_location_info_valid"),
    ("uint32_t", "chan_capture_status"),
    ("uint32_t", "rtt_che_buffer_pointer_low32"),
    ("uint32_t", "rtt_che_buffer_pointer_high8"),
    ("uint32_t", "buffer_addr_low32"),
    ("uint32_t", "buffer_addr_high32"),
    ("uint32_t", "srng_id"),
]

HEADER_GOLDEN = bytes.fromhex(
    "00302010011002100330201004302010053020100630201007302010"
    "0830201009070605040302010a070605040302010b3020100c302010"
    "0d07060504030201"
)
DBR_META_GOLDEN = bytes.fromhex(
    "00302010011002100330201004302010053020100630201007302010"
    "08302010093020100a100b100c100d100e100f101030201034373a3d"
    "4043464919101a105255585b5e6164676a6d7073767929102a102b10"
    "2c102d102e102f103010311032103310341035103610371038302010"
)
RX_PPDU_GOLDEN = bytes.fromhex(
    "00302010013020100230201003302010043020100530201006302010"
    "073020100830201009302010"
)


def extract_fields(header: str, struct_name: str):
    match = re.search(
        rf"struct\s+{re.escape(struct_name)}\s*\{{(.*?)\}}\s*"
        rf"(?:__packed|__attribute__\s*\(\(__packed__\)\))?;",
        header,
        re.S,
    )
    if not match:
        raise AssertionError(f"missing packed structure {struct_name}")

    fields = []
    for field_type, field_name in re.findall(
        r"^\s*(uint8_t|uint16_t|uint32_t|uint64_t|__le16|__le32|__le64)\s+(\w+)\s*;",
        match.group(1),
        re.M,
    ):
        fields.append((field_type, field_name))
    return fields


def sample_value(field_type: str, index: int):
    if field_type == "uint8_t":
        return (index * 3 + 1) & 0xFF
    if field_type in {"uint16_t", "__le16"}:
        return (0x1000 + index) & 0xFFFF
    if field_type in {"uint32_t", "__le32"}:
        return 0x10203000 + index
    if field_type in {"uint64_t", "__le64"}:
        return 0x0102030405060700 + index
    raise AssertionError(f"unsupported field type {field_type}")


def serialize(fields):
    layout = "<" + "".join(TYPE_FORMAT[field_type] for field_type, _ in fields)
    values = [sample_value(field_type, index)
              for index, (field_type, _) in enumerate(fields)]
    return struct.pack(layout, *values)


def require_le_assignments(source: str, object_name: str, fields):
    for field_type, field_name in fields:
        if not field_type.startswith("__le") or field_name.startswith("reserved"):
            continue
        pattern = (
            rf"\b{re.escape(object_name)}(?:->|\.){re.escape(field_name)}"
            rf"\s*=\s*(.*?);"
        )
        assignments = re.findall(pattern, source, re.S)
        if not assignments:
            continue
        width = field_type.removeprefix("__le")
        expected = f"cpu_to_le{width}"
        for assignment in assignments:
            if expected not in assignment and assignment.strip() not in {
                "0", "0U", "0ULL"
            }:
                raise AssertionError(
                    f"{object_name}.{field_name} lacks {expected}: "
                    f"{assignment.strip()}"
                )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source-root",
        type=Path,
        default=Path(__file__).resolve().parents[4],
        help="kernel source root",
    )
    args = parser.parse_args()
    root = args.source_root.resolve()

    header_path = root / (
        "drivers/staging/qca-wifi-host-cmn/umac/cfr/core/inc/cfr_defs_i.h"
    )
    target_path = root / (
        "drivers/staging/qca-wifi-host-cmn/target_if/cfr/src/"
        "target_if_cfr_enh.c"
    )
    core_path = root / (
        "drivers/staging/qca-wifi-host-cmn/umac/cfr/core/src/cfr_common.c"
    )

    header = header_path.read_text(encoding="utf-8")
    target = target_path.read_text(encoding="utf-8")
    core = core_path.read_text(encoding="utf-8")

    declared_header = extract_fields(header, "cfr_streamfs_record_hdr")
    declared_dbr = extract_fields(header, "cfr_streamfs_dbr_meta_v1")
    declared_rx = extract_fields(target, "cfr_rx_ppdu_snapshot")

    assert declared_header == HEADER_FIELDS
    assert declared_dbr == DBR_META_FIELDS
    assert declared_rx == RX_PPDU_FIELDS

    assert serialize(HEADER_FIELDS) == HEADER_GOLDEN
    assert serialize(DBR_META_FIELDS) == DBR_META_GOLDEN
    assert serialize(RX_PPDU_FIELDS) == RX_PPDU_GOLDEN
    assert len(DBR_META_GOLDEN) == 112
    assert len(RX_PPDU_GOLDEN) == 40
    assert len(HEADER_GOLDEN) == 64

    require_le_assignments(core, "hdr", HEADER_FIELDS)
    require_le_assignments(target, "meta", DBR_META_FIELDS)
    require_le_assignments(target, "rx_snapshot", RX_PPDU_FIELDS)

    print("qca6490 CFRR golden ABI: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
