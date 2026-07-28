// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "qti_nfc_function_lib.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_DEVICE "/dev/nq-nci"
#define DEFAULT_TIMEOUT_MS 2000U

static const char *transport_name(uint32_t transport)
{
    switch (transport) {
    case QTI_NFC_FUNCTION_TRANSPORT_KERNEL:
        return "kernel";
    case QTI_NFC_FUNCTION_TRANSPORT_NCI:
        return "nci";
    case QTI_NFC_FUNCTION_TRANSPORT_FW_DOWNLOAD:
        return "download";
    default:
        return "auto";
    }
}

static const char *binding_name(uint32_t binding)
{
    switch (binding) {
    case QTI_NFC_FUNCTION_BINDING_DIRECT:
        return "direct";
    case QTI_NFC_FUNCTION_BINDING_BUILTIN_REQUEST:
        return "built-in";
    default:
        return "payload";
    }
}

static int parse_transport(const char *text, uint32_t *transport)
{
    if (strcmp(text, "auto") == 0)
        *transport = QTI_NFC_FUNCTION_TRANSPORT_AUTO;
    else if (strcmp(text, "kernel") == 0)
        *transport = QTI_NFC_FUNCTION_TRANSPORT_KERNEL;
    else if (strcmp(text, "nci") == 0)
        *transport = QTI_NFC_FUNCTION_TRANSPORT_NCI;
    else if (strcmp(text, "download") == 0 || strcmp(text, "fwdl") == 0)
        *transport = QTI_NFC_FUNCTION_TRANSPORT_FW_DOWNLOAD;
    else
        return -EINVAL;
    return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno || end == text || *end != '\0' || parsed > UINT32_MAX)
        return -EINVAL;
    *value = (uint32_t)parsed;
    return 0;
}

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  qti-nfc-function list\n"
            "  qti-nfc-function info [options]\n"
            "  qti-nfc-function state [options]\n"
            "  qti-nfc-function call FUNCTION [options]\n"
            "  qti-nfc-function inspect-firmware LIBSN100U_FW.SO\n"
            "  qti-nfc-function upload-firmware LIBSN100U_FW.SO [options]\n"
            "\n"
            "Options:\n"
            "  --device PATH             default /dev/nq-nci\n"
            "  --transport MODE          auto, kernel, nci, download\n"
            "  --request HEX             request bytes\n"
            "  --request-file PATH       request bytes from a file\n"
            "  --timeout MS              transaction timeout, max 60000\n"
            "  --response-capacity N     maximum returned bytes\n"
            "  --no-response             write without waiting for response\n"
            "  --retry-write             use the driver's retry path\n"
            "  --record PATH             write framed NFCF records\n");
}

static int run_simple_call(struct qti_nfc_function_client *client,
                           struct qti_nfc_function_call *call,
                           uint32_t function, uint32_t transport,
                           uint32_t flags, const void *request,
                           uint32_t request_len, uint32_t response_capacity)
{
    memset(call, 0, sizeof(*call));
    call->function = function;
    call->transport = transport;
    call->flags = flags;
    call->request_len = request_len;
    call->response_capacity = response_capacity;
    if (request_len && request)
        memcpy(call->request, request, request_len);
    return qti_nfc_function_call(client, call);
}

static int list_functions(void)
{
    const struct qti_nfc_function_descriptor *catalog;
    size_t count;
    size_t i;

    catalog = qti_nfc_function_catalog(&count);
    for (i = 0; i < count; i++)
        printf("0x%04" PRIx32 " %-36s %-8s %s\n", catalog[i].id,
               catalog[i].name, transport_name(catalog[i].transport),
               binding_name(catalog[i].binding));
    return 0;
}

static int inspect_firmware(const char *path)
{
    uint8_t frame[QTI_NFC_FUNCTION_MAX_FRAME];
    uint8_t *sequence = NULL;
    size_t sequence_len = 0;
    size_t offset = 0;
    size_t records = 0;
    size_t record_len;
    size_t frame_len;
    size_t first_frame_len = 0;
    size_t last_frame_len = 0;
    int ret;

    ret = qti_nfc_function_load_firmware_sequence(path, &sequence,
                                                   &sequence_len);
    if (ret)
        return ret;
    while (offset < sequence_len) {
        ret = qti_nfc_function_build_download_frame(
                sequence + offset, sequence_len - offset, frame, sizeof(frame),
                &record_len, &frame_len);
        if (ret)
            break;
        if (!records)
            first_frame_len = frame_len;
        last_frame_len = frame_len;
        offset += record_len;
        records++;
    }
    if (!ret && offset != sequence_len)
        ret = -EPROTO;
    if (!ret) {
        printf("sequence_bytes=%zu\nrecords=%zu\nfirst_frame_bytes=%zu\n"
               "last_frame_bytes=%zu\n", sequence_len, records,
               first_frame_len, last_frame_len);
    }
    free(sequence);
    return ret;
}

static void print_abi(const struct qti_nfc_function_abi *abi)
{
    printf("abi=%u.%u\n", abi->abi_major, abi->abi_minor);
    printf("capabilities=0x%08" PRIx32 "\n", abi->capabilities);
    printf("max_data_len=%" PRIu32 "\n", abi->max_data_len);
    printf("max_nci_frame_len=%" PRIu32 "\n", abi->max_nci_frame_len);
    printf("max_frame_len=%" PRIu32 "\n", abi->max_frame_len);
    printf("max_timeout_ms=%" PRIu32 "\n", abi->max_timeout_ms);
    printf("max_function_id=0x%04" PRIx32 "\n", abi->max_function_id);
}

static void print_runtime_state(const struct qti_nfc_function_call *call)
{
    const struct qti_nfc_function_runtime_state *state;

    if (call->response_len < sizeof(*state)) {
        fprintf(stderr, "short runtime-state response: %" PRIu32 "\n",
                call->response_len);
        return;
    }
    state = (const void *)call->response;
    printf("mode=%u\n", state->mode);
    printf("controller_state=%u\n", state->controller_state);
    printf("ven=%u\nfirm=%u\nirq=%u\nclkreq=%u\n", state->ven,
           state->firm, state->irq, state->clkreq);
    printf("regulator_enabled=%u\nnfc_enabled=%u\n", state->regulator_enabled,
           state->nfc_enabled);
    printf("ese_powered=%u\nese_state=%u\nrf_field=%u\n",
           state->ese_powered, state->ese_state, state->rf_field);
    printf("interface_type=%u\n", state->interface_type);
    printf("chip_type=0x%02x\nrom_version=0x%02x\n", state->chip_type,
           state->rom_version);
    printf("firmware=%u.%u\n", state->fw_major, state->fw_minor);
}

static const struct qti_nfc_function_descriptor *resolve_function(
        const char *text, struct qti_nfc_function_descriptor *numeric)
{
    const struct qti_nfc_function_descriptor *descriptor;
    uint32_t id;

    descriptor = qti_nfc_function_by_name(text);
    if (descriptor)
        return descriptor;
    if (parse_u32(text, &id) != 0)
        return NULL;
    descriptor = qti_nfc_function_by_id(id);
    if (descriptor)
        return descriptor;
    numeric->id = id;
    numeric->name = text;
    numeric->transport = QTI_NFC_FUNCTION_TRANSPORT_AUTO;
    numeric->binding = QTI_NFC_FUNCTION_BINDING_BACKEND_PAYLOAD;
    return numeric;
}

int main(int argc, char **argv)
{
    struct qti_nfc_function_descriptor numeric;
    const struct qti_nfc_function_descriptor *descriptor = NULL;
    struct qti_nfc_function_client client;
    struct qti_nfc_function_call *call = NULL;
    struct qti_nfc_function_abi abi;
    const char *device = DEFAULT_DEVICE;
    const char *record = NULL;
    const char *request_hex = NULL;
    const char *request_file = NULL;
    const char *firmware_path = NULL;
    uint32_t timeout_ms = DEFAULT_TIMEOUT_MS;
    uint32_t response_capacity = 0;
    uint32_t transport = QTI_NFC_FUNCTION_TRANSPORT_AUTO;
    bool transport_set = false;
    bool expect_response = true;
    bool retry_write = false;
    bool state_command = false;
    bool upload_command = false;
    bool download_mode_active = false;
    uint8_t *firmware_sequence = NULL;
    size_t firmware_sequence_len = 0;
    size_t request_len = 0;
    int argi;
    int ret;
    int close_ret;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "list") == 0)
        return list_functions();
    if (strcmp(argv[1], "inspect-firmware") == 0) {
        if (argc != 3) {
            usage(stderr);
            return 2;
        }
        ret = inspect_firmware(argv[2]);
        if (ret) {
            fprintf(stderr, "inspect firmware: %s\n", strerror(-ret));
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "info") == 0) {
        descriptor = NULL;
        argi = 2;
    } else if (strcmp(argv[1], "state") == 0) {
        descriptor = qti_nfc_function_by_name("get-runtime-state");
        state_command = true;
        argi = 2;
    } else if (strcmp(argv[1], "call") == 0) {
        if (argc < 3) {
            usage(stderr);
            return 2;
        }
        descriptor = resolve_function(argv[2], &numeric);
        if (!descriptor) {
            fprintf(stderr, "unknown function: %s\n", argv[2]);
            return 2;
        }
        argi = 3;
    } else if (strcmp(argv[1], "upload-firmware") == 0) {
        if (argc < 3) {
            usage(stderr);
            return 2;
        }
        upload_command = true;
        firmware_path = argv[2];
        descriptor = qti_nfc_function_by_name("download-write");
        argi = 3;
    } else {
        usage(stderr);
        return 2;
    }

    while (argi < argc) {
        const char *option = argv[argi++];

        if (strcmp(option, "--device") == 0 && argi < argc)
            device = argv[argi++];
        else if (strcmp(option, "--transport") == 0 && argi < argc) {
            ret = parse_transport(argv[argi++], &transport);
            if (ret) {
                fprintf(stderr, "invalid transport\n");
                return 2;
            }
            transport_set = true;
        } else if (strcmp(option, "--request") == 0 && argi < argc)
            request_hex = argv[argi++];
        else if (strcmp(option, "--request-file") == 0 && argi < argc)
            request_file = argv[argi++];
        else if (strcmp(option, "--timeout") == 0 && argi < argc) {
            if (parse_u32(argv[argi++], &timeout_ms) != 0 ||
                timeout_ms > QTI_NFC_FUNCTION_MAX_TIMEOUT_MS) {
                fprintf(stderr, "invalid timeout\n");
                return 2;
            }
        } else if (strcmp(option, "--response-capacity") == 0 && argi < argc) {
            if (parse_u32(argv[argi++], &response_capacity) != 0 ||
                response_capacity > QTI_NFC_FUNCTION_MAX_DATA) {
                fprintf(stderr, "invalid response capacity\n");
                return 2;
            }
        } else if (strcmp(option, "--no-response") == 0) {
            expect_response = false;
        } else if (strcmp(option, "--retry-write") == 0) {
            retry_write = true;
        } else if (strcmp(option, "--record") == 0 && argi < argc) {
            record = argv[argi++];
        } else {
            fprintf(stderr, "invalid or incomplete option: %s\n", option);
            return 2;
        }
    }

    if (request_hex && request_file) {
        fprintf(stderr, "use only one request source\n");
        return 2;
    }
    if (upload_command && (request_hex || request_file)) {
        fprintf(stderr, "upload-firmware reads requests from the firmware library\n");
        return 2;
    }

    if (upload_command) {
        ret = qti_nfc_function_load_firmware_sequence(firmware_path,
                                                       &firmware_sequence,
                                                       &firmware_sequence_len);
        if (ret) {
            fprintf(stderr, "load firmware sequence: %s\n", strerror(-ret));
            return 1;
        }
    }

    ret = qti_nfc_function_open(&client, device, timeout_ms, record);
    if (ret) {
        fprintf(stderr, "open gateway: %s\n", strerror(-ret));
        free(firmware_sequence);
        return 1;
    }

    if (!descriptor) {
        ret = qti_nfc_function_get_abi(&client, &abi);
        if (!ret)
            print_abi(&abi);
        goto close;
    }

    call = calloc(1, sizeof(*call));
    if (!call) {
        ret = -ENOMEM;
        goto close;
    }

    if (upload_command) {
        size_t offset = 0;
        size_t records = 0;
        uint8_t session_state;
        uint8_t lifecycle_state;
        uint16_t image_version;
        uint16_t installed_version;
        uint32_t crc_status;

        if (firmware_sequence_len < 6) {
            ret = -EPROTO;
            goto close;
        }
        image_version = (uint16_t)firmware_sequence[4] |
                        (uint16_t)firmware_sequence[5] << 8;

        ret = run_simple_call(&client, call,
                              QTI_NFC_FUNCTION_ENTER_DOWNLOAD_MODE,
                              QTI_NFC_FUNCTION_TRANSPORT_KERNEL, 0,
                              NULL, 0, 0);
        if (ret)
            goto close;
        download_mode_active = true;
        ret = run_simple_call(&client, call,
                              QTI_NFC_FUNCTION_GET_DOWNLOAD_SESSION,
                              QTI_NFC_FUNCTION_TRANSPORT_FW_DOWNLOAD,
                              QTI_NFC_FUNCTION_CALL_F_EXPECT_RESPONSE |
                              QTI_NFC_FUNCTION_CALL_F_RETRY_WRITE,
                              NULL, 0, QTI_NFC_FUNCTION_MAX_FRAME);
        if (ret)
            goto close;
        ret = qti_nfc_function_parse_download_session(
                call->response, call->response_len, &session_state,
                &lifecycle_state);
        if (ret)
            goto close;
        printf("initial_download_session_state=0x%02x\n"
               "initial_lifecycle_state=0x%02x\n", session_state,
               lifecycle_state);
        if (session_state != 0U) {
            ret = -EUCLEAN;
            goto close;
        }

        while (offset < firmware_sequence_len) {
            uint8_t frame[QTI_NFC_FUNCTION_MAX_FRAME];
            size_t record_len;
            size_t frame_len;

            ret = qti_nfc_function_build_download_frame(
                    firmware_sequence + offset, firmware_sequence_len - offset,
                    frame, sizeof(frame), &record_len, &frame_len);
            if (ret)
                goto close;
            {
                unsigned int retry;

                for (retry = 0; retry < 3; retry++) {
                    ret = run_simple_call(&client, call,
                            QTI_NFC_FUNCTION_DOWNLOAD_WRITE,
                            QTI_NFC_FUNCTION_TRANSPORT_FW_DOWNLOAD,
                            QTI_NFC_FUNCTION_CALL_F_EXPECT_RESPONSE |
                            QTI_NFC_FUNCTION_CALL_F_RETRY_WRITE,
                            frame, (uint32_t)frame_len,
                            QTI_NFC_FUNCTION_MAX_FRAME);
                    if (ret != -EBUSY)
                        break;
                    {
                        const struct timespec wait = { .tv_nsec = 50000000L };
                        nanosleep(&wait, NULL);
                    }
                }
            }
            if (ret)
                goto close;
            offset += record_len;
            records++;
            if ((records % 16U) == 0U || offset == firmware_sequence_len)
                printf("uploaded_records=%zu uploaded_bytes=%zu/%zu\n",
                       records, offset, firmware_sequence_len);
        }
        printf("firmware_records_transmitted=%zu\n", records);

        ret = run_simple_call(&client, call,
                              QTI_NFC_FUNCTION_GET_DOWNLOAD_SESSION,
                              QTI_NFC_FUNCTION_TRANSPORT_FW_DOWNLOAD,
                              QTI_NFC_FUNCTION_CALL_F_EXPECT_RESPONSE |
                              QTI_NFC_FUNCTION_CALL_F_RETRY_WRITE,
                              NULL, 0, QTI_NFC_FUNCTION_MAX_FRAME);
        if (ret)
            goto close;
        ret = qti_nfc_function_parse_download_session(
                call->response, call->response_len, &session_state,
                &lifecycle_state);
        if (ret)
            goto close;
        printf("download_session_state=0x%02x\n"
               "post_upload_lifecycle_state=0x%02x\n", session_state,
               lifecycle_state);
        if (session_state != 0U) {
            ret = -EINPROGRESS;
            goto close;
        }

        ret = run_simple_call(&client, call,
                              QTI_NFC_FUNCTION_GET_FIRMWARE_VERSION,
                              QTI_NFC_FUNCTION_TRANSPORT_FW_DOWNLOAD,
                              QTI_NFC_FUNCTION_CALL_F_EXPECT_RESPONSE |
                              QTI_NFC_FUNCTION_CALL_F_RETRY_WRITE,
                              NULL, 0, QTI_NFC_FUNCTION_MAX_FRAME);
        if (ret)
            goto close;
        ret = qti_nfc_function_parse_download_version(
                call->response, call->response_len, &installed_version);
        if (ret)
            goto close;
        printf("image_firmware_version=0x%04x\n"
               "installed_firmware_version=0x%04x\n", image_version,
               installed_version);
        if (installed_version != image_version) {
            ret = -ESTALE;
            goto close;
        }

        ret = run_simple_call(&client, call,
                              QTI_NFC_FUNCTION_DOWNLOAD_CHECK_INTEGRITY,
                              QTI_NFC_FUNCTION_TRANSPORT_FW_DOWNLOAD,
                              QTI_NFC_FUNCTION_CALL_F_EXPECT_RESPONSE |
                              QTI_NFC_FUNCTION_CALL_F_RETRY_WRITE,
                              NULL, 0, QTI_NFC_FUNCTION_MAX_FRAME);
        if (ret)
            goto close;
        ret = qti_nfc_function_parse_download_integrity(
                call->response, call->response_len, &crc_status);
        if (ret)
            goto close;
        printf("firmware_integrity_crc_status=0x%08" PRIx32 "\n",
               crc_status);
        goto close;
    }

    call->function = descriptor->id;
    call->transport = transport_set ? transport : descriptor->transport;
    call->timeout_ms = (int32_t)timeout_ms;
    call->flags = expect_response ? QTI_NFC_FUNCTION_CALL_F_EXPECT_RESPONSE : 0;
    if (retry_write)
        call->flags |= QTI_NFC_FUNCTION_CALL_F_RETRY_WRITE;
    if (!response_capacity) {
        if (!expect_response)
            response_capacity = 0;
        else if (call->transport == QTI_NFC_FUNCTION_TRANSPORT_KERNEL)
            response_capacity = QTI_NFC_FUNCTION_MAX_DATA;
        else if (call->transport == QTI_NFC_FUNCTION_TRANSPORT_FW_DOWNLOAD ||
                 call->transport == QTI_NFC_FUNCTION_TRANSPORT_AUTO)
            response_capacity = QTI_NFC_FUNCTION_MAX_FRAME;
        else
            response_capacity = QTI_NFC_FUNCTION_MAX_NCI_FRAME;
    }
    call->response_capacity = response_capacity;

    if (request_hex) {
        ret = qti_nfc_function_parse_hex(request_hex, call->request,
                                         sizeof(call->request), &request_len);
        if (ret)
            goto close;
    } else if (request_file) {
        ret = qti_nfc_function_read_file(request_file, call->request,
                                         sizeof(call->request), &request_len);
        if (ret)
            goto close;
    }
    call->request_len = (uint32_t)request_len;

    ret = qti_nfc_function_call(&client, call);
    printf("function=%s\nid=0x%04" PRIx32 "\ntransport=%s\nstatus=%" PRId32
           "\nsequence=%" PRIu32 "\nresponse_len=%" PRIu32 "\n",
           descriptor->name, descriptor->id, transport_name(call->transport),
           call->status, call->sequence, call->response_len);
    if (state_command && !ret)
        print_runtime_state(call);
    else if (call->response_len) {
        fputs("response=", stdout);
        qti_nfc_function_print_hex(stdout, call->response, call->response_len);
    }

close:
    if (download_mode_active) {
        int cleanup_ret = run_simple_call(&client, call,
                              QTI_NFC_FUNCTION_EXIT_DOWNLOAD_MODE,
                              QTI_NFC_FUNCTION_TRANSPORT_KERNEL, 0,
                              NULL, 0, 0);
        if (!ret)
            ret = cleanup_ret;
    }
    free(firmware_sequence);
    free(call);
    close_ret = qti_nfc_function_close(&client);
    if (!ret)
        ret = close_ret;
    if (ret) {
        fprintf(stderr, "operation failed: %s (%d)\n", strerror(-ret), ret);
        return 1;
    }
    return 0;
}
