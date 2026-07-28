#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Source and ABI tests for the QTI SN100U named function gateway."""

import argparse
import re
import subprocess
import tempfile
from pathlib import Path


def run(command, cwd=None):
    return subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 \
                else (crc << 1) & 0xFFFF
    return crc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--firmware",
        type=Path,
        default=Path("/home/ubuntu/kernel-compile/nfc-infos/libsn100u_fw.so"),
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    tools = root / "nfc_tools"
    header = root / "include/uapi/linux/nfc/qti_nfc_function.h"
    catalog = tools / "qti_nfc_function_catalog.inc"
    source = (root / "drivers/nfc/qti/nfc_common.c").read_text()
    i2c_source = (root / "drivers/nfc/qti/nfc_i2c_drv.c").read_text()
    common_header = (root / "drivers/nfc/qti/nfc_common.h").read_text()
    header_text = header.read_text()

    enum_match = re.search(
        r"enum\s+qti_nfc_function_id\s*\{(.*?)\n\};", header_text, re.S
    )
    require(enum_match, "missing function enum")
    entries = re.findall(
        r"QTI_NFC_FUNCTION_([A-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)",
        enum_match.group(1),
    )
    entries = [(name, int(value, 0)) for name, value in entries]
    public_entries = [(name, value) for name, value in entries
                      if name != "UNSPECIFIED"]
    require(len(public_entries) == 149,
            f"expected 149 functions, got {len(public_entries)}")
    require(len({value for _, value in entries}) == len(entries),
            "duplicate function IDs")

    catalog_entries = re.findall(
        r"QTI_NFC_FUNCTION_ENTRY\(QTI_NFC_FUNCTION_([A-Z0-9_]+),\s*"
        r'"([a-z0-9-]+)",\s*(QTI_NFC_FUNCTION_TRANSPORT_[A-Z_]+),\s*'
        r'(QTI_NFC_FUNCTION_BINDING_[A-Z_]+)\)',
        catalog.read_text(),
    )
    require(len(catalog_entries) == len(public_entries),
            "catalog count does not match UAPI")
    require({name for name, _, _, _ in catalog_entries} == {
        name for name, _ in public_entries
    }, "catalog identifiers do not match UAPI")
    require(len({public for _, public, _, _ in catalog_entries}) ==
            len(catalog_entries), "duplicate public function names")
    receive_entry = next(entry for entry in catalog_entries
                         if entry[0] == "RECEIVE_FRAME")
    require(receive_entry[2] == "QTI_NFC_FUNCTION_TRANSPORT_AUTO",
            "receive-frame must select transport from controller mode")

    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        generated = temp / "catalog.inc"
        generated_rst = temp / "functions.rst"
        run(
            [
                "python3",
                str(tools / "generate-function-catalog.py"),
                str(header),
                str(generated),
                "--rst",
                str(generated_rst),
            ]
        )
        require(generated.read_bytes() == catalog.read_bytes(), "stale catalog")
        require(generated_rst.read_bytes() == (
            root / "Documentation/nfc/qti-sn100u-functions.rst"
        ).read_bytes(), "stale function document")

        abi_test = temp / "abi_test.c"
        abi_test.write_text(
            f'''#include "{header}"
_Static_assert(sizeof(struct qti_nfc_function_record_header) == 56, "record ABI");
_Static_assert(sizeof(struct qti_nfc_function_runtime_state) == 16, "state ABI");
_Static_assert(sizeof(struct qti_nfc_function_gpio_value) == 8, "gpio ABI");
_Static_assert(sizeof(struct qti_nfc_function_call) < 16384, "ioctl size");
_Static_assert(_IOC_SIZE(QTI_NFC_FUNCTION_CALL) == sizeof(struct qti_nfc_function_call), "ioctl encoding");
int main(void) {{ return 0; }}
'''
        )
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(abi_test), "-o", str(temp / "abi_test")])
        run([str(temp / "abi_test")])

        file_test = temp / "file_test.c"
        file_test.write_text(
            f'''#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "{tools / 'qti_nfc_function_lib.h'}"

static uint16_t crc16(const unsigned char *data, size_t len) {{
    uint16_t crc = 0xffffU;
    size_t i;
    unsigned int bit;
    for (i = 0; i < len; i++) {{
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000U) ?
                  (uint16_t)((crc << 1) ^ 0x1021U) :
                  (uint16_t)(crc << 1);
    }}
    return crc;
}}

static size_t make_frame(const unsigned char *payload, size_t payload_len,
                         unsigned char *frame) {{
    uint16_t crc;
    frame[0] = (unsigned char)(payload_len >> 8);
    frame[1] = (unsigned char)payload_len;
    memcpy(frame + 2, payload, payload_len);
    crc = crc16(frame, payload_len + 2);
    frame[payload_len + 2] = (unsigned char)(crc >> 8);
    frame[payload_len + 3] = (unsigned char)crc;
    return payload_len + 4;
}}

int main(int argc, char **argv) {{
    unsigned char data[QTI_NFC_FUNCTION_MAX_DATA];
    unsigned char frame[32];
    unsigned char session[] = {{ 0x00, 0x00, 0x00, 0x11 }};
    unsigned char version[8] = {{ 0 }};
    unsigned char wrong_version[12] = {{ 0 }};
    unsigned char integrity[] = {{ 0x00, 28, 4, 0, 0x0f, 0xc0, 0x3f, 0xff }};
    unsigned char bad_integrity[] = {{ 0x00, 28, 4, 0, 0x0f, 0, 0, 0 }};
    unsigned char bad_lifecycle[] = {{ 0x00, 0x00, 0x00, 0x22 }};
    unsigned char short_session[] = {{ 0x00, 0x00, 0x11 }};
    unsigned char fragment[] = {{ 0x2d }};
    size_t len = 0;
    size_t frame_len;
    uint8_t session_state;
    uint8_t lifecycle_state;
    uint16_t firmware_version;
    uint32_t crc_status;
    int ret;
    if (argc != 2) return 2;
    ret = qti_nfc_function_read_file(argv[1], data, sizeof(data), &len);
    if (ret || len != sizeof(data)) return 3;
    frame_len = make_frame(session, sizeof(session), frame);
    if (qti_nfc_function_parse_download_session(
            frame, frame_len, &session_state, &lifecycle_state) ||
        session_state != 0 || lifecycle_state != 0x11) return 4;
    frame[frame_len - 1] ^= 1;
    if (qti_nfc_function_validate_download_response(frame, frame_len) !=
        -EBADMSG) return 5;
    frame_len = make_frame(bad_lifecycle, sizeof(bad_lifecycle), frame);
    if (qti_nfc_function_parse_download_session(
            frame, frame_len, &session_state, &lifecycle_state) !=
        -EPERM) return 6;
    frame_len = make_frame(short_session, sizeof(short_session), frame);
    if (qti_nfc_function_parse_download_session(
            frame, frame_len, &session_state, &lifecycle_state) !=
        -EPROTO) return 7;
    version[4] = 0x34;
    version[5] = 0x12;
    frame_len = make_frame(version, sizeof(version), frame);
    if (qti_nfc_function_parse_download_version(
            frame, frame_len, &firmware_version) ||
        firmware_version != 0x1234) return 8;
    frame_len = make_frame(wrong_version, sizeof(wrong_version), frame);
    if (qti_nfc_function_parse_download_version(
            frame, frame_len, &firmware_version) != -EPROTO) return 9;
    frame_len = make_frame(integrity, sizeof(integrity), frame);
    if (qti_nfc_function_parse_download_integrity(
            frame, frame_len, &crc_status) ||
        crc_status != 0xff3fc00fU) return 10;
    frame_len = make_frame(bad_integrity, sizeof(bad_integrity), frame);
    if (qti_nfc_function_parse_download_integrity(
            frame, frame_len, &crc_status) != -EBADMSG) return 11;
    frame_len = make_frame(fragment, sizeof(fragment), frame);
    if (qti_nfc_function_validate_download_response(frame, frame_len) !=
        -EINPROGRESS) return 12;
    return 0;
}}
'''
        )
        exact_file = temp / "exact.bin"
        exact_file.write_bytes(bytes(4096))
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(file_test), str(tools / "qti_nfc_function_lib.c"),
             "-o", str(temp / "file_test")])
        run([str(temp / "file_test"), str(exact_file)])

    required_kernel_cases = {
        "QTI_NFC_FUNCTION_SESSION_OPEN",
        "QTI_NFC_FUNCTION_SESSION_CLOSE",
        "QTI_NFC_FUNCTION_CALL",
        "QTI_NFC_FUNCTION_ENTER_DOWNLOAD_MODE",
        "QTI_NFC_FUNCTION_EXIT_DOWNLOAD_MODE",
        "QTI_NFC_FUNCTION_GET_FIRMWARE_VERSION",
        "QTI_NFC_FUNCTION_GET_DOWNLOAD_SESSION",
        "QTI_NFC_FUNCTION_DOWNLOAD_CHECK_INTEGRITY",
        "QTI_NFC_FUNCTION_GET_ROUTING",
        "QTI_NFC_FUNCTION_ESE_POWER_ON",
        "QTI_NFC_FUNCTION_TRACE_READ",
    }
    missing = sorted(item for item in required_kernel_cases if item not in source)
    require(not missing, f"missing kernel cases: {missing}")
    kernel_bound = {
        name for name, _, _, binding in catalog_entries
        if binding != "QTI_NFC_FUNCTION_BINDING_BACKEND_PAYLOAD"
    }
    missing_dispatch = sorted(
        name for name in kernel_bound
        if f"case QTI_NFC_FUNCTION_{name}:" not in source
    )
    require(not missing_dispatch,
            f"kernel-bound catalog entries lack dispatch cases: {missing_dispatch}")
    require("WRITE_ONCE(nfc_dev->read_owner, filp)" in i2c_source and
            "WRITE_ONCE(nfc_dev->read_owner, NULL)" in i2c_source and
            "READ_ONCE(nfc_dev->read_owner) != pfile" in source,
            "per-file NFC read ownership is not enforced")
    require("case NFC_SET_RESET_READ_PENDING:" in source and
            "case NFC_GET_GPIO_STATUS:" in source,
            "NXP SN100U HAL compatibility ioctls are missing")
    require("NFC_SET_RESET_READ_PENDING _IOW(NFC_MAGIC, 0x04, unsigned int)" in
            common_header and
            "NFC_GET_GPIO_STATUS\t_IOR(NFC_MAGIC, 0x05, unsigned int)" in
            common_header,
            "NXP SN100U HAL compatibility ioctl values changed")
    for array_name, command_id in {
        "dl_get_version": 0xF1,
        "dl_get_session": 0xF2,
        "dl_check_integrity": 0xE0,
    }.items():
        command_match = re.search(
            rf"static const u8 {array_name}\[\]\s*=\s*\{{(.*?)\}};",
            source, re.S,
        )
        require(command_match, f"missing built-in command {array_name}")
        command = bytes(int(value, 16) for value in re.findall(
            r"0x([0-9a-fA-F]{1,2})", command_match.group(1)
        ))
        require(len(command) == 8 and command[:2] == b"\x00\x04" and
                command[2] == command_id,
                f"invalid built-in command layout: {array_name}")
        require(crc16(command[:-2]) == int.from_bytes(command[-2:], "big"),
                f"invalid built-in command CRC: {array_name}")

    run(["make", "-C", str(tools), "clean", "all"])
    listed = run([str(tools / "qti-nfc-function"), "list"]).stdout.splitlines()
    require(len(listed) == len(public_entries), "CLI catalog count mismatch")

    if args.firmware.exists():
        output = run(
            [str(tools / "qti-nfc-function"), "inspect-firmware",
             str(args.firmware)]
        ).stdout
        expected = {
            "sequence_bytes=204518",
            "records=371",
            "first_frame_bytes=312",
            "last_frame_bytes=522",
        }
        require(expected.issubset(set(output.splitlines())), output)

        blob = args.firmware.read_bytes()
        sequence = blob[0x360:0x32246]
        records = []
        offset = 0
        while offset < len(sequence):
            payload_len = int.from_bytes(sequence[offset:offset + 2], "big")
            source_len = 2 + payload_len
            require(payload_len > 0 and offset + source_len <= len(sequence),
                    "invalid independent firmware record parse")
            records.append(sequence[offset:offset + source_len])
            offset += source_len
        require(offset == len(sequence) and len(records) == 371,
                "firmware records do not exactly consume sequence")
        expected_crcs = {
            0: 0xC0B7,
            1: 0x2849,
            2: 0x866C,
            369: 0x76D3,
            370: 0xB3FF,
        }
        for index, expected_crc in expected_crcs.items():
            require(crc16(records[index]) == expected_crc,
                    f"record {index} CRC mismatch")

        with tempfile.TemporaryDirectory() as malformed_temp:
            malformed = Path(malformed_temp) / "truncated.so"
            malformed.write_bytes(blob[:1024])
            result = subprocess.run(
                [str(tools / "qti-nfc-function"), "inspect-firmware",
                 str(malformed)], text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            require(result.returncode != 0, "truncated ELF was accepted")

    print(f"QTI NFC function ABI: PASS ({len(public_entries)} named functions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
