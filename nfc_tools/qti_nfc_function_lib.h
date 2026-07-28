/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QTI_NFC_FUNCTION_LIB_H
#define QTI_NFC_FUNCTION_LIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../include/uapi/linux/nfc/qti_nfc_function.h"

struct qti_nfc_function_descriptor {
    uint32_t id;
    const char *name;
    uint32_t transport;
    uint32_t binding;
};

enum qti_nfc_function_binding {
    QTI_NFC_FUNCTION_BINDING_DIRECT = 1,
    QTI_NFC_FUNCTION_BINDING_BUILTIN_REQUEST = 2,
    QTI_NFC_FUNCTION_BINDING_BACKEND_PAYLOAD = 3,
};

struct qti_nfc_function_client {
    int fd;
    uint32_t session_id;
    uint32_t sequence;
    FILE *record_file;
};

const struct qti_nfc_function_descriptor *qti_nfc_function_catalog(size_t *count);
const struct qti_nfc_function_descriptor *qti_nfc_function_by_name(const char *name);
const struct qti_nfc_function_descriptor *qti_nfc_function_by_id(uint32_t id);

int qti_nfc_function_open(struct qti_nfc_function_client *client,
                          const char *device, uint32_t timeout_ms,
                          const char *record_path);
int qti_nfc_function_get_abi(struct qti_nfc_function_client *client,
                             struct qti_nfc_function_abi *abi);
int qti_nfc_function_call(struct qti_nfc_function_client *client,
                          struct qti_nfc_function_call *call);
int qti_nfc_function_close(struct qti_nfc_function_client *client);
int qti_nfc_function_parse_hex(const char *text, uint8_t *out,
                               size_t capacity, size_t *out_len);
int qti_nfc_function_read_file(const char *path, uint8_t *out,
                               size_t capacity, size_t *out_len);
int qti_nfc_function_load_firmware_sequence(const char *path, uint8_t **sequence,
                                            size_t *sequence_len);
int qti_nfc_function_build_download_frame(const uint8_t *record,
                                          size_t record_available,
                                          uint8_t *frame,
                                          size_t frame_capacity,
                                          size_t *record_len,
                                          size_t *frame_len);
int qti_nfc_function_validate_download_response(const uint8_t *response,
                                                size_t response_len);
int qti_nfc_function_parse_download_session(const uint8_t *response,
                                            size_t response_len,
                                            uint8_t *session_state,
                                            uint8_t *lifecycle_state);
int qti_nfc_function_parse_download_version(const uint8_t *response,
                                            size_t response_len,
                                            uint16_t *firmware_version);
int qti_nfc_function_parse_download_integrity(const uint8_t *response,
                                              size_t response_len,
                                              uint32_t *crc_status);
void qti_nfc_function_print_hex(FILE *stream, const uint8_t *data, size_t len);

#endif
