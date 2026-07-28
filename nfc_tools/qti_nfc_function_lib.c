// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "qti_nfc_function_lib.h"

#include <ctype.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define cpu_to_le16(value) ((uint16_t)(value))
#define cpu_to_le32(value) ((uint32_t)(value))
#define cpu_to_le64(value) ((uint64_t)(value))
#else
#define cpu_to_le16(value) __builtin_bswap16((uint16_t)(value))
#define cpu_to_le32(value) __builtin_bswap32((uint32_t)(value))
#define cpu_to_le64(value) __builtin_bswap64((uint64_t)(value))
#endif

static uint16_t download_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffffU;
    size_t i;
    unsigned int bit;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) :
                                    (uint16_t)(crc << 1);
    }
    return crc;
}

#define QTI_NFC_FUNCTION_ENTRY(id, name, transport, binding) \
    { id, name, transport, binding },
static const struct qti_nfc_function_descriptor function_catalog[] = {
#include "qti_nfc_function_catalog.inc"
};
#undef QTI_NFC_FUNCTION_ENTRY

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int write_record(FILE *file, uint32_t type, uint32_t session_id,
                        uint32_t sequence, uint32_t function,
                        uint32_t transport, uint32_t flags, int32_t status,
                        uint32_t timeout_ms,
                        const void *payload, uint32_t payload_len)
{
    struct qti_nfc_function_record_header header;

    if (!file)
        return 0;

    memset(&header, 0, sizeof(header));
    header.magic = cpu_to_le32(QTI_NFC_FUNCTION_RECORD_MAGIC);
    header.version = cpu_to_le16(QTI_NFC_FUNCTION_RECORD_VERSION);
    header.header_len = cpu_to_le16((uint16_t)sizeof(header));
    header.type = cpu_to_le32(type);
    header.session_id = cpu_to_le32(session_id);
    header.sequence = cpu_to_le32(sequence);
    header.function = cpu_to_le32(function);
    header.transport = cpu_to_le32(transport);
    header.flags = cpu_to_le32(flags);
    header.payload_len = cpu_to_le32(payload_len);
    header.status = cpu_to_le32((uint32_t)status);
    header.timeout_ms = cpu_to_le32(timeout_ms);
    header.timestamp_ns = cpu_to_le64(monotonic_ns());

    if (fwrite(&header, 1, sizeof(header), file) != sizeof(header))
        return -EIO;
    if (payload_len &&
        fwrite(payload, 1, payload_len, file) != payload_len)
        return -EIO;
    if (fflush(file) != 0)
        return -errno;
    return 0;
}

const struct qti_nfc_function_descriptor *qti_nfc_function_catalog(size_t *count)
{
    if (count)
        *count = sizeof(function_catalog) / sizeof(function_catalog[0]);
    return function_catalog;
}

const struct qti_nfc_function_descriptor *qti_nfc_function_by_name(const char *name)
{
    size_t i;

    if (!name)
        return NULL;
    for (i = 0; i < sizeof(function_catalog) / sizeof(function_catalog[0]); i++) {
        if (strcmp(function_catalog[i].name, name) == 0)
            return &function_catalog[i];
    }
    return NULL;
}

const struct qti_nfc_function_descriptor *qti_nfc_function_by_id(uint32_t id)
{
    size_t i;

    for (i = 0; i < sizeof(function_catalog) / sizeof(function_catalog[0]); i++) {
        if (function_catalog[i].id == id)
            return &function_catalog[i];
    }
    return NULL;
}

int qti_nfc_function_get_abi(struct qti_nfc_function_client *client,
                             struct qti_nfc_function_abi *abi)
{
    if (!client || client->fd < 0 || !abi)
        return -EINVAL;
    memset(abi, 0, sizeof(*abi));
    if (ioctl(client->fd, QTI_NFC_FUNCTION_GET_ABI, abi) != 0)
        return -errno;
    if (abi->abi_major != QTI_NFC_FUNCTION_ABI_MAJOR ||
        abi->size < sizeof(*abi))
        return -EPROTO;
    return 0;
}

int qti_nfc_function_open(struct qti_nfc_function_client *client,
                          const char *device, uint32_t timeout_ms,
                          const char *record_path)
{
    struct qti_nfc_function_session session;
    struct qti_nfc_function_abi abi;
    int ret;

    if (!client || !device || timeout_ms > QTI_NFC_FUNCTION_MAX_TIMEOUT_MS)
        return -EINVAL;

    memset(client, 0, sizeof(*client));
    client->fd = -1;
    client->fd = open(device, O_RDWR | O_CLOEXEC);
    if (client->fd < 0)
        return -errno;

    ret = qti_nfc_function_get_abi(client, &abi);
    if (ret)
        goto error;

    if (record_path) {
        client->record_file = fopen(record_path, "wb");
        if (!client->record_file) {
            ret = -errno;
            goto error;
        }
    }

    memset(&session, 0, sizeof(session));
    session.abi_major = QTI_NFC_FUNCTION_ABI_MAJOR;
    session.abi_minor = QTI_NFC_FUNCTION_ABI_MINOR;
    session.size = sizeof(session);
    session.default_timeout_ms = timeout_ms;
    if (ioctl(client->fd, QTI_NFC_FUNCTION_SESSION_OPEN, &session) != 0) {
        ret = -errno;
        goto error;
    }
    client->session_id = session.session_id;
    ret = write_record(client->record_file,
                       QTI_NFC_FUNCTION_RECORD_SESSION_START,
                       client->session_id, 0, 0,
                       QTI_NFC_FUNCTION_TRANSPORT_KERNEL, session.flags, 0,
                       timeout_ms, &abi, sizeof(abi));
    if (ret) {
        ioctl(client->fd, QTI_NFC_FUNCTION_SESSION_CLOSE, &session);
        client->session_id = 0;
        goto error;
    }
    return 0;

error:
    if (client->record_file) {
        fclose(client->record_file);
        client->record_file = NULL;
    }
    close(client->fd);
    client->fd = -1;
    return ret;
}

int qti_nfc_function_call(struct qti_nfc_function_client *client,
                          struct qti_nfc_function_call *call)
{
    int ret;

    if (!client || client->fd < 0 || !client->session_id || !call)
        return -EINVAL;
    if (call->request_len > QTI_NFC_FUNCTION_MAX_DATA ||
        call->response_capacity > QTI_NFC_FUNCTION_MAX_DATA)
        return -EMSGSIZE;

    call->abi_major = QTI_NFC_FUNCTION_ABI_MAJOR;
    call->abi_minor = QTI_NFC_FUNCTION_ABI_MINOR;
    call->size = sizeof(*call);
    call->session_id = client->session_id;
    if (!call->sequence)
        call->sequence = ++client->sequence;

    ret = write_record(client->record_file, QTI_NFC_FUNCTION_RECORD_CALL,
                       client->session_id, call->sequence, call->function,
                       call->transport, call->flags, 0,
                       (uint32_t)call->timeout_ms,
                       call->request, call->request_len);
    if (ret)
        return ret;

    if (ioctl(client->fd, QTI_NFC_FUNCTION_CALL, call) != 0) {
        ret = -errno;
        write_record(client->record_file, QTI_NFC_FUNCTION_RECORD_ERROR,
                     client->session_id, call->sequence, call->function,
                     call->transport, call->flags, ret,
                     (uint32_t)call->timeout_ms, NULL, 0);
        return ret;
    }

    ret = write_record(client->record_file,
                       call->status ? QTI_NFC_FUNCTION_RECORD_ERROR :
                       (call->function == QTI_NFC_FUNCTION_RECEIVE_FRAME ||
                        call->function == QTI_NFC_FUNCTION_READ_ONLY) ?
                                      QTI_NFC_FUNCTION_RECORD_EVENT :
                                      QTI_NFC_FUNCTION_RECORD_RESULT,
                       client->session_id, call->sequence, call->function,
                       call->transport, call->flags, call->status,
                       (uint32_t)call->timeout_ms, call->response,
                       call->response_len);
    if (ret)
        return ret;
    return call->status;
}

int qti_nfc_function_close(struct qti_nfc_function_client *client)
{
    struct qti_nfc_function_session session;
    int ret = 0;

    if (!client)
        return -EINVAL;
    if (client->fd >= 0 && client->session_id) {
        memset(&session, 0, sizeof(session));
        session.abi_major = QTI_NFC_FUNCTION_ABI_MAJOR;
        session.abi_minor = QTI_NFC_FUNCTION_ABI_MINOR;
        session.size = sizeof(session);
        session.session_id = client->session_id;
        if (ioctl(client->fd, QTI_NFC_FUNCTION_SESSION_CLOSE, &session) != 0)
            ret = -errno;
        {
            int record_ret = write_record(client->record_file,
                    QTI_NFC_FUNCTION_RECORD_SESSION_END,
                    client->session_id, client->sequence, 0,
                    QTI_NFC_FUNCTION_TRANSPORT_KERNEL, session.flags, ret,
                    0, NULL, 0);
            if (!ret)
                ret = record_ret;
        }
    }
    if (client->record_file) {
        if (fclose(client->record_file) != 0 && !ret)
            ret = -errno;
        client->record_file = NULL;
    }
    if (client->fd >= 0)
        close(client->fd);
    client->fd = -1;
    client->session_id = 0;
    return ret;
}

int qti_nfc_function_parse_hex(const char *text, uint8_t *out,
                               size_t capacity, size_t *out_len)
{
    int high = -1;
    size_t len = 0;

    if (!text || !out || !out_len)
        return -EINVAL;

    while (*text) {
        int value;
        unsigned char c = (unsigned char)*text++;

        if (isspace(c) || c == ':' || c == '-' || c == ',')
            continue;
        if (c >= '0' && c <= '9')
            value = c - '0';
        else if (c >= 'a' && c <= 'f')
            value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            value = c - 'A' + 10;
        else
            return -EINVAL;

        if (high < 0) {
            high = value;
        } else {
            if (len >= capacity)
                return -EMSGSIZE;
            out[len++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }
    if (high >= 0)
        return -EINVAL;
    *out_len = len;
    return 0;
}

int qti_nfc_function_read_file(const char *path, uint8_t *out,
                               size_t capacity, size_t *out_len)
{
    FILE *file;
    size_t len;

    if (!path || !out || !out_len)
        return -EINVAL;
    file = fopen(path, "rb");
    if (!file)
        return -errno;
    len = fread(out, 1, capacity, file);
    if (ferror(file)) {
        int ret = -EIO;
        fclose(file);
        return ret;
    }
    if (fgetc(file) != EOF) {
        fclose(file);
        return -EMSGSIZE;
    }
    if (ferror(file)) {
        fclose(file);
        return -EIO;
    }
    fclose(file);
    *out_len = len;
    return 0;
}

static int range_valid(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static int elf_symbol_range(const Elf64_Shdr *sections, size_t section_count,
                            const Elf64_Sym *symbol, size_t file_len,
                            size_t *offset, size_t *length)
{
    const Elf64_Shdr *section;
    uint64_t delta;

    if (!sections || !symbol || !offset || !length ||
        symbol->st_shndx >= section_count || !symbol->st_size)
        return -EINVAL;
    section = &sections[symbol->st_shndx];
    if (section->sh_type == SHT_NOBITS || symbol->st_value < section->sh_addr)
        return -EINVAL;
    delta = symbol->st_value - section->sh_addr;
    if (delta > section->sh_size || symbol->st_size > section->sh_size - delta ||
        delta > SIZE_MAX || section->sh_offset > SIZE_MAX - (size_t)delta)
        return -EINVAL;
    *offset = section->sh_offset + (size_t)delta;
    *length = symbol->st_size;
    return range_valid(*offset, *length, file_len) ? 0 : -EINVAL;
}

int qti_nfc_function_load_firmware_sequence(const char *path, uint8_t **sequence,
                                            size_t *sequence_len)
{
    const char sequence_symbol[] = "gphDnldNfc_DlSequence";
    const char size_symbol[] = "gphDnldNfc_DlSeqSz";
    const Elf64_Shdr *sections;
    const Elf64_Shdr *dynsym = NULL;
    const Elf64_Shdr *dynstr;
    const Elf64_Sym *symbols;
    const Elf64_Sym *sequence_entry = NULL;
    const Elf64_Sym *size_entry = NULL;
    const Elf64_Ehdr *ehdr;
    const char *strings;
    uint8_t *file_data = NULL;
    uint8_t *result = NULL;
    size_t file_len;
    size_t symbol_count;
    size_t sequence_offset;
    size_t sequence_size;
    size_t size_offset;
    size_t size_size;
    size_t i;
    FILE *file;
    long end;
    int ret = -EINVAL;

    if (!path || !sequence || !sequence_len)
        return -EINVAL;
    *sequence = NULL;
    *sequence_len = 0;

    file = fopen(path, "rb");
    if (!file)
        return -errno;
    if (fseek(file, 0, SEEK_END) != 0 || (end = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        ret = -EIO;
        goto out_file;
    }
    file_len = (size_t)end;
    if (file_len < sizeof(Elf64_Ehdr))
        goto out_file;
    file_data = malloc(file_len);
    if (!file_data) {
        ret = -ENOMEM;
        goto out_file;
    }
    if (fread(file_data, 1, file_len, file) != file_len) {
        ret = -EIO;
        goto out;
    }

    ehdr = (const void *)file_data;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_shentsize != sizeof(Elf64_Shdr) ||
        !range_valid(ehdr->e_shoff,
                     (size_t)ehdr->e_shnum * sizeof(Elf64_Shdr), file_len))
        goto out;
    sections = (const void *)(file_data + ehdr->e_shoff);
    for (i = 0; i < ehdr->e_shnum; i++) {
        if (sections[i].sh_type == SHT_DYNSYM) {
            dynsym = &sections[i];
            break;
        }
    }
    if (!dynsym || dynsym->sh_link >= ehdr->e_shnum ||
        dynsym->sh_entsize != sizeof(Elf64_Sym) ||
        !range_valid(dynsym->sh_offset, dynsym->sh_size, file_len))
        goto out;
    dynstr = &sections[dynsym->sh_link];
    if (!range_valid(dynstr->sh_offset, dynstr->sh_size, file_len))
        goto out;
    strings = (const char *)(file_data + dynstr->sh_offset);
    symbols = (const void *)(file_data + dynsym->sh_offset);
    symbol_count = dynsym->sh_size / sizeof(Elf64_Sym);

    for (i = 0; i < symbol_count; i++) {
        const Elf64_Sym *symbol = &symbols[i];
        const char *name;
        size_t name_remaining;

        if (symbol->st_name >= dynstr->sh_size)
            continue;
        name = strings + symbol->st_name;
        name_remaining = dynstr->sh_size - symbol->st_name;
        if (!memchr(name, '\0', name_remaining))
            goto out;
        if (strcmp(name, sequence_symbol) == 0)
            sequence_entry = symbol;
        else if (strcmp(name, size_symbol) == 0)
            size_entry = symbol;
    }

    if (!sequence_entry || !size_entry ||
        elf_symbol_range(sections, ehdr->e_shnum, sequence_entry, file_len,
                         &sequence_offset, &sequence_size) != 0 ||
        elf_symbol_range(sections, ehdr->e_shnum, size_entry, file_len,
                         &size_offset, &size_size) != 0 || size_size < 4)
        goto out;
    if (((uint32_t)file_data[size_offset] |
         (uint32_t)file_data[size_offset + 1] << 8 |
         (uint32_t)file_data[size_offset + 2] << 16 |
         (uint32_t)file_data[size_offset + 3] << 24) != sequence_size)
        goto out;

    result = malloc(sequence_size);
    if (!result) {
        ret = -ENOMEM;
        goto out;
    }
    memcpy(result, file_data + sequence_offset, sequence_size);
    *sequence = result;
    *sequence_len = sequence_size;
    result = NULL;
    ret = 0;

out:
    free(result);
    free(file_data);
out_file:
    fclose(file);
    return ret;
}

int qti_nfc_function_build_download_frame(const uint8_t *record,
                                          size_t record_available,
                                          uint8_t *frame,
                                          size_t frame_capacity,
                                          size_t *record_len,
                                          size_t *frame_len)
{
    size_t payload_len;
    size_t source_len;
    uint16_t crc;

    if (!record || !frame || !record_len || !frame_len || record_available < 2)
        return -EINVAL;
    payload_len = ((size_t)record[0] << 8) | record[1];
    source_len = 2 + payload_len;
    if (!payload_len || source_len > record_available ||
        source_len + 2 > frame_capacity ||
        source_len + 2 > QTI_NFC_FUNCTION_MAX_FRAME)
        return -EMSGSIZE;

    memcpy(frame, record, source_len);
    crc = download_crc16(frame, source_len);
    frame[source_len] = (uint8_t)(crc >> 8);
    frame[source_len + 1] = (uint8_t)crc;
    *record_len = source_len;
    *frame_len = source_len + 2;
    return 0;
}

int qti_nfc_function_validate_download_response(const uint8_t *response,
                                                size_t response_len)
{
    size_t payload_len;
    size_t frame_len;
    uint16_t expected_crc;
    uint16_t received_crc;

    if (!response || response_len < 5)
        return -EPROTO;
    payload_len = ((size_t)response[0] << 8) | response[1];
    frame_len = 2 + payload_len + 2;
    if (!payload_len || frame_len != response_len)
        return -EPROTO;
    expected_crc = download_crc16(response, 2 + payload_len);
    received_crc = (uint16_t)response[frame_len - 2] << 8 |
                   response[frame_len - 1];
    if (expected_crc != received_crc)
        return -EBADMSG;

    switch (response[2]) {
    case 0x00:
        return 0;
    case 0x20:
        return -EBUSY;
    case 0x21:
        return -EKEYREJECTED;
    case 0x24:
        return -EALREADY;
    case 0x2d:
    case 0x2e:
        return -EINPROGRESS;
    default:
        return -EREMOTEIO;
    }
}

int qti_nfc_function_parse_download_session(const uint8_t *response,
                                            size_t response_len,
                                            uint8_t *session_state,
                                            uint8_t *lifecycle_state)
{
    int ret;

    if (!session_state || !lifecycle_state)
        return -EINVAL;
    ret = qti_nfc_function_validate_download_response(response, response_len);
    if (ret)
        return ret;
    if (response_len != 8 || response[0] != 0 || response[1] != 4)
        return -EPROTO;
    *session_state = response[3];
    *lifecycle_state = response[5];
    if (*lifecycle_state != 0x00 && *lifecycle_state != 0x11)
        return -EPERM;
    return 0;
}

int qti_nfc_function_parse_download_version(const uint8_t *response,
                                            size_t response_len,
                                            uint16_t *firmware_version)
{
    int ret;

    if (!firmware_version)
        return -EINVAL;
    ret = qti_nfc_function_validate_download_response(response, response_len);
    if (ret)
        return ret;
    /* Marble SN1xx returns status plus seven version bytes. */
    if (response_len != 12 || response[0] != 0 || response[1] != 8)
        return -EPROTO;
    *firmware_version = (uint16_t)response[6] |
                        (uint16_t)response[7] << 8;
    return 0;
}

int qti_nfc_function_parse_download_integrity(const uint8_t *response,
                                              size_t response_len,
                                              uint32_t *crc_status)
{
    const uint32_t acceptable_crc_status = 0xff3fc00fU;
    uint32_t status;
    int ret;

    if (!crc_status)
        return -EINVAL;
    ret = qti_nfc_function_validate_download_response(response, response_len);
    if (ret)
        return ret;
    if (response_len < 12 || response[0] != 0 || response[1] < 8 ||
        response[3] > 28 || response[4] > 4)
        return -EPROTO;
    status = (uint32_t)response[6] |
             (uint32_t)response[7] << 8 |
             (uint32_t)response[8] << 16 |
             (uint32_t)response[9] << 24;
    *crc_status = status;
    return (status & acceptable_crc_status) == acceptable_crc_status ?
           0 : -EBADMSG;
}

void qti_nfc_function_print_hex(FILE *stream, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        fprintf(stream, "%02x", data[i]);
    fputc('\n', stream);
}
