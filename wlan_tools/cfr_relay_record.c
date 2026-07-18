// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define CFRR_MAGIC 0x52524643U
#define CFRR_V1 1U
#define CFRR_V2 2U
#define CFRR_V1_HDR_LEN 40U
#define CFRR_V2_HDR_LEN 64U
#define CFRR_MAX_HDR_LEN 256U

#define CFRR_TYPE_FINAL 0x00000000U
#define CFRR_TYPE_RAW_DBR 0x80000000U
#define CFRR_TYPE_RX_PPDU 0x80000001U
#define CFRR_TYPE_DBR_META 0x80000002U
#define CFRR_TYPE_SESSION_START 0x80000003U
#define CFRR_TYPE_SESSION_END 0x80000004U
#define CFRR_TYPE_REARM 0x80000005U

#define DEFAULT_INPUT "/sys/kernel/debug/cfrwlan0/cfr_dump0"
#define DEFAULT_CONTROL "/sys/kernel/qca6490/cfr_control"
#define DEFAULT_OUTPUT "/data/local/tmp/cfr_capture.cfrr"
#define DEFAULT_SECONDS 30ULL
#define DEFAULT_MAX_MB 64ULL
#define DEFAULT_READ_SIZE (256U * 1024U)
#define DEFAULT_MAX_FRAME (32U * 1024U)
#define DEFAULT_MAX_RESYNC (1024U * 1024U)
#define DEFAULT_STATS_SECONDS 5ULL
#define MAX_ROTATED_SEGMENTS 1024U

struct options {
    const char *input;
    const char *output;
    const char *metadata;
    const char *control;
    uint64_t seconds;
    uint64_t max_bytes;
    uint64_t max_records;
    uint64_t rotate_bytes;
    uint64_t rotate_seconds;
    uint64_t stats_seconds;
    uint64_t max_frame_bytes;
    uint64_t max_resync_bytes;
    size_t read_size;
    bool relay_reset;
    bool relay_on;
    bool relay_off_on_exit;
    bool stop_on_session_end;
    bool no_metadata;
    bool follow;
    bool verbose;
};

struct stats {
    uint64_t input_bytes;
    uint64_t output_bytes;
    uint64_t records;
    uint64_t type_final;
    uint64_t type_raw_dbr;
    uint64_t type_rx_ppdu;
    uint64_t type_dbr_meta;
    uint64_t type_session_start;
    uint64_t type_session_end;
    uint64_t type_rearm;
    uint64_t type_unknown;
    uint64_t resync_bytes;
    uint64_t invalid_frames;
    uint64_t sequence_gaps;
    uint64_t sequence_resets;
    uint64_t out_of_order_records;
    uint64_t sessions_seen;
    uint64_t session_changes;
    uint64_t drain_records_discarded;
    uint64_t unprocessed_buffer_bytes;
    uint64_t trailing_buffer_bytes;
    uint64_t trailing_incomplete_frames;
    uint64_t eof_count;
    uint64_t read_calls;
    uint64_t write_calls;
    uint64_t min_frame_size;
    uint64_t max_frame_size;
    uint64_t session_id;
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t last_type;
    bool have_sequence;
    bool saw_session_end;
    const char *stop_reason;
};

struct buffer {
    uint8_t *data;
    size_t start;
    size_t end;
    size_t cap;
    size_t max_cap;
};

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_len;
};

struct segment {
    char path[PATH_MAX];
    char sha256[65];
    uint64_t bytes;
    uint64_t records;
    bool complete;
};

struct output {
    const struct options *opt;
    struct segment segments[MAX_ROTATED_SEGMENTS];
    struct sha256_ctx sha;
    uint32_t segment_count;
    int fd;
    uint64_t opened_ms;
};

static volatile sig_atomic_t stop_requested;

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static uint32_t rotr32(uint32_t value, unsigned int shift)
{
    return (value >> shift) | (value << (32U - shift));
}

static void sha256_transform(struct sha256_ctx *ctx, const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned int i;

    for (i = 0; i < 16; i++) {
        const uint8_t *p = block + i * 4U;
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2];
    d = ctx->state[3]; e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(struct sha256_ctx *ctx)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bit_count = 0;
    ctx->block_len = 0;
}

static void sha256_update(struct sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    ctx->bit_count += (uint64_t)len * 8U;
    while (len) {
        size_t copy = 64U - ctx->block_len;
        if (copy > len)
            copy = len;
        memcpy(ctx->block + ctx->block_len, data, copy);
        ctx->block_len += copy;
        data += copy;
        len -= copy;
        if (ctx->block_len == 64U) {
            sha256_transform(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, uint8_t digest[32])
{
    uint64_t bits = ctx->bit_count;
    unsigned int i;

    ctx->block[ctx->block_len++] = 0x80U;
    if (ctx->block_len > 56U) {
        memset(ctx->block + ctx->block_len, 0, 64U - ctx->block_len);
        sha256_transform(ctx, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56U - ctx->block_len);
    for (i = 0; i < 8; i++)
        ctx->block[63U - i] = (uint8_t)(bits >> (i * 8U));
    sha256_transform(ctx, ctx->block);
    for (i = 0; i < 8; i++) {
        digest[i * 4U] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4U + 2U] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4U + 3U] = (uint8_t)ctx->state[i];
    }
}

static uint64_t now_ms(clockid_t clock_id)
{
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint64_t now_real_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static bool parse_u64(const char *arg, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;
    if (!arg || !*arg || *arg == '-')
        return false;
    errno = 0;
    value = strtoull(arg, &end, 0);
    if (errno || end == arg || *end)
        return false;
    *out = value;
    return true;
}

static bool checked_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a && b > UINT64_MAX / a)
        return false;
    *out = a * b;
    return true;
}

static uint64_t deadline_ms(uint64_t start, uint64_t seconds)
{
    uint64_t duration;

    if (!checked_mul_u64(seconds, 1000ULL, &duration) ||
        duration > UINT64_MAX - start)
        return UINT64_MAX;
    return start + duration;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t written = write(fd, p, len);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!written) {
            errno = EIO;
            return -1;
        }
        p += written;
        len -= (size_t)written;
    }
    return 0;
}

static int write_text_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    int ret;
    if (fd < 0)
        return -1;
    ret = write_all(fd, text, strlen(text));
    if (close(fd) != 0 && ret == 0)
        ret = -1;
    return ret;
}

static int relay_command(const struct options *opt, const char *command)
{
    if (!opt->control)
        return 0;
    if (opt->verbose)
        fprintf(stderr, "control: %s", command);
    return write_text_file(opt->control, command);
}

static bool same_existing_file(const char *first, const char *second)
{
    struct stat first_stat, second_stat;

    if (!first || !second || stat(first, &first_stat) != 0 ||
        stat(second, &second_stat) != 0)
        return false;
    return first_stat.st_dev == second_stat.st_dev &&
           first_stat.st_ino == second_stat.st_ino;
}

static size_t buffer_len(const struct buffer *buf)
{
    return buf->end - buf->start;
}

static void buffer_consume(struct buffer *buf, size_t len)
{
    if (len >= buffer_len(buf)) {
        buf->start = 0;
        buf->end = 0;
    } else {
        buf->start += len;
    }
}

static int buffer_append(struct buffer *buf, const uint8_t *data, size_t len)
{
    size_t used = buffer_len(buf);
    if (len > buf->max_cap - used) {
        errno = EMSGSIZE;
        return -1;
    }
    if (buf->cap - buf->end < len && buf->start) {
        memmove(buf->data, buf->data + buf->start, used);
        buf->start = 0;
        buf->end = used;
    }
    if (buf->cap - buf->end < len) {
        size_t need = buf->end + len;
        size_t cap = buf->cap ? buf->cap : 65536U;
        uint8_t *new_data;
        while (cap < need && cap < buf->max_cap)
            cap = cap > buf->max_cap / 2U ? buf->max_cap : cap * 2U;
        if (cap < need) {
            errno = EMSGSIZE;
            return -1;
        }
        new_data = realloc(buf->data, cap);
        if (!new_data)
            return -1;
        buf->data = new_data;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->end, data, len);
    buf->end += len;
    return 0;
}

static size_t find_magic(const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i + 4U <= len; i++)
        if (get_le32(data + i) == CFRR_MAGIC)
            return i;
    return SIZE_MAX;
}

static const char *type_name(uint32_t type)
{
    switch (type) {
    case CFRR_TYPE_FINAL: return "final";
    case CFRR_TYPE_RAW_DBR: return "raw_dbr";
    case CFRR_TYPE_RX_PPDU: return "rx_ppdu";
    case CFRR_TYPE_DBR_META: return "dbr_meta";
    case CFRR_TYPE_SESSION_START: return "session_start";
    case CFRR_TYPE_SESSION_END: return "session_end";
    case CFRR_TYPE_REARM: return "rearm";
    default: return "unknown";
    }
}

static void count_type(struct stats *stats, uint32_t type)
{
    switch (type) {
    case CFRR_TYPE_FINAL: stats->type_final++; break;
    case CFRR_TYPE_RAW_DBR: stats->type_raw_dbr++; break;
    case CFRR_TYPE_RX_PPDU: stats->type_rx_ppdu++; break;
    case CFRR_TYPE_DBR_META: stats->type_dbr_meta++; break;
    case CFRR_TYPE_SESSION_START: stats->type_session_start++; break;
    case CFRR_TYPE_SESSION_END: stats->type_session_end++; break;
    case CFRR_TYPE_REARM: stats->type_rearm++; break;
    default: stats->type_unknown++; break;
    }
}

static int output_open_segment(struct output *out)
{
    struct segment *segment;
    char checksum_path[PATH_MAX];
    char default_metadata_path[PATH_MAX];
    const char *metadata_path = out->opt->metadata;
    struct stat path_stat;
    int ret;
    bool rotate = out->opt->rotate_bytes || out->opt->rotate_seconds;

    if (out->segment_count >= MAX_ROTATED_SEGMENTS) {
        errno = EFBIG;
        return -1;
    }
    segment = &out->segments[out->segment_count];
    if (rotate)
        ret = snprintf(segment->path, sizeof(segment->path), "%s.%06u.cfrr",
                       out->opt->output, out->segment_count);
    else
        ret = snprintf(segment->path, sizeof(segment->path), "%s",
                       out->opt->output);
    if (ret < 0 || (size_t)ret >= sizeof(segment->path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    ret = snprintf(checksum_path, sizeof(checksum_path), "%s.sha256",
                   segment->path);
    if (ret < 0 || (size_t)ret >= sizeof(checksum_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!out->opt->no_metadata && !metadata_path) {
        ret = snprintf(default_metadata_path, sizeof(default_metadata_path),
                       "%s.json", out->opt->output);
        if (ret < 0 || (size_t)ret >= sizeof(default_metadata_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        metadata_path = default_metadata_path;
    }
    if ((metadata_path &&
         (!strcmp(segment->path, metadata_path) ||
          !strcmp(checksum_path, metadata_path) ||
          !strcmp(out->opt->input, metadata_path) ||
          same_existing_file(out->opt->input, metadata_path))) ||
        !strcmp(segment->path, out->opt->input) ||
        !strcmp(checksum_path, out->opt->input) ||
        same_existing_file(segment->path, out->opt->input) ||
        same_existing_file(checksum_path, out->opt->input)) {
        errno = EINVAL;
        return -1;
    }
    if ((lstat(segment->path, &path_stat) == 0 &&
         S_ISLNK(path_stat.st_mode)) ||
        (lstat(checksum_path, &path_stat) == 0 &&
         S_ISLNK(path_stat.st_mode))) {
        errno = ELOOP;
        return -1;
    }
    out->fd = open(segment->path,
                   O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                   0600);
    if (out->fd < 0)
        return -1;
    if (unlink(checksum_path) != 0 && errno != ENOENT) {
        int saved_errno = errno;
        (void)close(out->fd);
        out->fd = -1;
        errno = saved_errno;
        return -1;
    }
    if (ftruncate(out->fd, 0) != 0) {
        int saved_errno = errno;
        (void)close(out->fd);
        out->fd = -1;
        errno = saved_errno;
        return -1;
    }
    sha256_init(&out->sha);
    segment->complete = true;
    out->opened_ms = now_ms(CLOCK_MONOTONIC);
    out->segment_count++;
    return 0;
}

static int output_close_segment(struct output *out)
{
    struct segment *segment;
    uint8_t digest[32];
    char checksum_path[PATH_MAX];
    char line[PATH_MAX + 80U];
    unsigned int i;
    int fd, ret, saved_errno = 0;

    if (out->fd < 0 || !out->segment_count)
        return 0;
    if (fsync(out->fd) != 0)
        saved_errno = errno;
    if (close(out->fd) != 0 && !saved_errno)
        saved_errno = errno;
    out->fd = -1;
    segment = &out->segments[out->segment_count - 1U];
    sha256_final(&out->sha, digest);
    for (i = 0; i < 32U; i++)
        (void)snprintf(segment->sha256 + i * 2U, 3U, "%02x", digest[i]);
    if (saved_errno) {
        segment->complete = false;
        errno = saved_errno;
        return -1;
    }
    ret = snprintf(checksum_path, sizeof(checksum_path), "%s.sha256",
                   segment->path);
    if (ret < 0 || (size_t)ret >= sizeof(checksum_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    ret = snprintf(line, sizeof(line), "%s  %s\n", segment->sha256,
                   segment->path);
    if (ret < 0 || (size_t)ret >= sizeof(line)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(checksum_path,
              O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    ret = write_all(fd, line, strlen(line));
    if (close(fd) != 0 && ret == 0)
        ret = -1;
    return ret;
}

static int output_write_frame(struct output *out, const uint8_t *frame,
                              size_t frame_len)
{
    struct segment *segment;
    const uint8_t *cursor = frame;
    size_t remaining = frame_len;
    uint64_t now = now_ms(CLOCK_MONOTONIC);
    bool rotate = false;

    if (out->fd < 0 && output_open_segment(out) != 0)
        return -1;
    segment = &out->segments[out->segment_count - 1U];
    if (segment->bytes) {
        if (out->opt->rotate_bytes &&
            (segment->bytes >= out->opt->rotate_bytes ||
             frame_len > out->opt->rotate_bytes - segment->bytes))
            rotate = true;
        if (out->opt->rotate_seconds &&
            now >= deadline_ms(out->opened_ms, out->opt->rotate_seconds))
            rotate = true;
    }
    if (rotate) {
        if (output_close_segment(out) != 0 || output_open_segment(out) != 0)
            return -1;
        segment = &out->segments[out->segment_count - 1U];
    }
    while (remaining) {
        ssize_t written = write(out->fd, cursor, remaining);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            segment->complete = false;
            return -1;
        }
        if (!written) {
            segment->complete = false;
            errno = EIO;
            return -1;
        }
        sha256_update(&out->sha, cursor, (size_t)written);
        segment->bytes += (uint64_t)written;
        cursor += written;
        remaining -= (size_t)written;
    }
    segment->records++;
    return 0;
}

static void track_sequence(struct stats *stats, uint32_t seq,
                           uint64_t session_id, bool session_start)
{
    uint32_t expected, delta;
    bool new_session = session_start;

    if (stats->have_sequence && session_id != stats->session_id)
        new_session = true;
    if (!stats->have_sequence || new_session) {
        if (stats->have_sequence) {
            stats->session_changes++;
        } else {
            stats->first_seq = seq;
        }
        stats->session_id = session_id;
        stats->last_seq = seq;
        stats->have_sequence = true;
        stats->sessions_seen++;
        return;
    }
    expected = stats->last_seq + 1U;
    delta = seq - expected;
    if (delta) {
        if (delta < 0x80000000U) {
            stats->sequence_gaps += delta;
        } else {
            stats->sequence_resets++;
            stats->out_of_order_records++;
            return;
        }
    }
    stats->last_seq = seq;
}

static bool valid_session_payload(uint32_t type, const uint8_t *payload,
                                  uint32_t payload_len)
{
    uint16_t version, declared_len;
    uint32_t minimum_len;

    if (payload_len < 4U)
        return false;
    version = get_le16(payload);
    declared_len = get_le16(payload + 2U);
    if (type == CFRR_TYPE_SESSION_START) {
        if (version == 1U)
            minimum_len = 64U;
        else if (version == 2U)
            minimum_len = 80U;
        else
            return false;
    } else if (type == CFRR_TYPE_SESSION_END) {
        if (version != 1U)
            return false;
        minimum_len = 80U;
    } else {
        return false;
    }
    return payload_len >= minimum_len && declared_len >= minimum_len &&
           declared_len <= payload_len;
}

/* Returns 1 to continue, 0 for a requested clean stop, and -1 on error. */
static int process_buffer(struct buffer *buf, struct output *out,
                           const struct options *opt, struct stats *stats,
                           bool drain_to_session_end)
{
    while (buffer_len(buf)) {
        uint8_t *frame = buf->data + buf->start;
        size_t available = buffer_len(buf);
        size_t pos = find_magic(frame, available);
        uint16_t version, hdr_len;
        uint32_t type, seq, payload_len;
        uint64_t frame_len, session_id = 0;
        bool session_payload_ok, exceeds_limit;

        if (pos == SIZE_MAX) {
            size_t drop = available > 3U ? available - 3U : 0U;
            stats->resync_bytes += drop;
            buffer_consume(buf, drop);
            if (stats->resync_bytes > opt->max_resync_bytes)
                goto resync_limit;
            break;
        }
        if (pos) {
            stats->resync_bytes += pos;
            buffer_consume(buf, pos);
            if (stats->resync_bytes > opt->max_resync_bytes)
                goto resync_limit;
            continue;
        }
        if (available < CFRR_V1_HDR_LEN)
            break;
        version = get_le16(frame + 4U);
        hdr_len = get_le16(frame + 6U);
        type = get_le32(frame + 8U);
        seq = get_le32(frame + 16U);
        payload_len = get_le32(frame + 20U);
        if (!version || hdr_len < CFRR_V1_HDR_LEN ||
            (version >= CFRR_V2 && hdr_len < CFRR_V2_HDR_LEN) ||
            hdr_len > CFRR_MAX_HDR_LEN ||
            payload_len > opt->max_frame_bytes ||
            (uint64_t)hdr_len + payload_len > opt->max_frame_bytes) {
            stats->invalid_frames++;
            stats->resync_bytes++;
            buffer_consume(buf, 1U);
            if (stats->resync_bytes > opt->max_resync_bytes)
                goto resync_limit;
            continue;
        }
        frame_len = (uint64_t)hdr_len + payload_len;
        if (frame_len > SIZE_MAX) {
            stats->invalid_frames++;
            buffer_consume(buf, 1U);
            continue;
        }
        if (available < (size_t)frame_len)
            break;
        if (version >= CFRR_V2)
            session_id = get_le64(frame + 40U);
        session_payload_ok = (version == CFRR_V1 || version == CFRR_V2) &&
            valid_session_payload(type, frame + hdr_len, payload_len);
        if ((type == CFRR_TYPE_SESSION_START ||
             type == CFRR_TYPE_SESSION_END) && version <= CFRR_V2 &&
            !session_payload_ok)
            stats->invalid_frames++;

        exceeds_limit = opt->max_records &&
                        stats->records >= opt->max_records;
        if (!exceeds_limit && opt->max_bytes)
            exceeds_limit = stats->output_bytes >= opt->max_bytes ||
                frame_len > opt->max_bytes - stats->output_bytes;
        if (exceeds_limit && !drain_to_session_end) {
            if (opt->max_records && stats->records >= opt->max_records)
                stats->stop_reason = "max_records";
            else
                stats->stop_reason = "max_bytes";
            return 0;
        }
        if (exceeds_limit && drain_to_session_end) {
            stats->drain_records_discarded++;
            buffer_consume(buf, (size_t)frame_len);
            if (type == CFRR_TYPE_SESSION_END && session_payload_ok) {
                stats->saw_session_end = true;
                return 0;
            }
            continue;
        }
        if (UINT64_MAX - stats->output_bytes < frame_len) {
            errno = EOVERFLOW;
            stats->stop_reason = "byte_counter_overflow";
            return -1;
        }
        if (output_write_frame(out, frame, (size_t)frame_len) != 0) {
            stats->stop_reason = "write_error";
            return -1;
        }
        track_sequence(stats, seq, session_id,
                       type == CFRR_TYPE_SESSION_START && session_payload_ok);
        stats->records++;
        stats->write_calls++;
        stats->output_bytes += frame_len;
        stats->last_type = type;
        if (!stats->min_frame_size || frame_len < stats->min_frame_size)
            stats->min_frame_size = frame_len;
        if (frame_len > stats->max_frame_size)
            stats->max_frame_size = frame_len;
        count_type(stats, type);
        if (type == CFRR_TYPE_SESSION_END && session_payload_ok)
            stats->saw_session_end = true;
        if (opt->verbose)
            fprintf(stderr, "record=%" PRIu64 " session=%" PRIu64
                    " seq=%u v=%u type=%s bytes=%" PRIu64 "\n",
                    stats->records, session_id, seq, version,
                    type_name(type), frame_len);
        buffer_consume(buf, (size_t)frame_len);
        if (type == CFRR_TYPE_SESSION_END && session_payload_ok &&
            (drain_to_session_end || opt->stop_on_session_end)) {
            if (!stats->stop_reason)
                stats->stop_reason = "session_end";
            return 0;
        }
    }
    if (stats->resync_bytes > opt->max_resync_bytes) {
resync_limit:
        stats->stop_reason = "resync_limit";
        errno = EBADMSG;
        return -1;
    }
    return 1;
}

static int report_reader_stats(const struct options *opt,
                               const struct stats *stats)
{
    char command[160];
    int ret;
    if (!opt->control)
        return 0;
    ret = snprintf(command, sizeof(command), "reader_stats %" PRIu64
                   " %" PRIu64 " %" PRIu64 "\n",
                   stats->sequence_gaps, stats->resync_bytes,
                   stats->invalid_frames);
    if (ret < 0 || (size_t)ret >= sizeof(command)) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_text_file(opt->control, command);
}

static void json_string(FILE *fp, const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    fputc('"', fp);
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', fp);
            fputc(*p, fp);
        } else if (*p < 0x20U) {
            fprintf(fp, "\\u%04x", *p);
        } else {
            fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

static int write_metadata(const struct options *opt, const struct stats *stats,
                           const struct output *out, uint64_t start_real_ns,
                           uint64_t end_real_ns, int exit_status,
                           uint64_t kernel_drops_before,
                           uint64_t kernel_drops_after)
{
    char default_path[PATH_MAX];
    const char *path = opt->metadata;
    struct utsname uts;
    FILE *fp;
    int fd;
    uint32_t i;
    int ret;
    uint64_t kernel_drops = kernel_drops_after >= kernel_drops_before ?
        kernel_drops_after - kernel_drops_before : kernel_drops_after;

    if (opt->no_metadata)
        return 0;
    if (!path) {
        ret = snprintf(default_path, sizeof(default_path), "%s.json",
                       opt->output);
        if (ret < 0 || (size_t)ret >= sizeof(default_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        path = default_path;
    }
    memset(&uts, 0, sizeof(uts));
    (void)uname(&uts);
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
              0600);
    if (fd < 0)
        return -1;
    fp = fdopen(fd, "w");
    if (!fp) {
        (void)close(fd);
        return -1;
    }
    fprintf(fp, "{\n  \"input\": "); json_string(fp, opt->input);
    fprintf(fp, ",\n  \"output_base\": "); json_string(fp, opt->output);
    fprintf(fp, ",\n  \"control\": "); json_string(fp, opt->control);
    fprintf(fp, ",\n  \"kernel_release\": "); json_string(fp, uts.release);
    fprintf(fp, ",\n  \"machine\": "); json_string(fp, uts.machine);
    fprintf(fp, ",\n  \"device_chip\": \"Marble WCN6856/QCA6490\",\n");
    fprintf(fp, "  \"start_real_ns\": %" PRIu64 ",\n", start_real_ns);
    fprintf(fp, "  \"end_real_ns\": %" PRIu64 ",\n", end_real_ns);
    fprintf(fp, "  \"exit_status\": %d,\n", exit_status);
    fprintf(fp, "  \"stop_reason\": "); json_string(fp, stats->stop_reason);
    fprintf(fp, ",\n  \"session_id\": %" PRIu64 ",\n", stats->session_id);
    fprintf(fp, "  \"records\": %" PRIu64 ",\n", stats->records);
    fprintf(fp, "  \"input_bytes\": %" PRIu64 ",\n", stats->input_bytes);
    fprintf(fp, "  \"output_bytes\": %" PRIu64 ",\n", stats->output_bytes);
    fprintf(fp, "  \"sequence_gaps\": %" PRIu64 ",\n", stats->sequence_gaps);
    fprintf(fp, "  \"sequence_resets\": %" PRIu64 ",\n", stats->sequence_resets);
    fprintf(fp, "  \"out_of_order_records\": %" PRIu64 ",\n",
            stats->out_of_order_records);
    fprintf(fp, "  \"sessions_seen\": %" PRIu64 ",\n", stats->sessions_seen);
    fprintf(fp, "  \"session_changes\": %" PRIu64 ",\n", stats->session_changes);
    fprintf(fp, "  \"resync_bytes\": %" PRIu64 ",\n", stats->resync_bytes);
    fprintf(fp, "  \"invalid_frames\": %" PRIu64 ",\n", stats->invalid_frames);
    fprintf(fp, "  \"kernel_reported_drops\": %" PRIu64 ",\n", kernel_drops);
    fprintf(fp, "  \"kernel_drops_before\": %" PRIu64 ",\n",
            kernel_drops_before);
    fprintf(fp, "  \"kernel_drops_after\": %" PRIu64 ",\n",
            kernel_drops_after);
    fprintf(fp, "  \"first_sequence\": %u,\n", stats->first_seq);
    fprintf(fp, "  \"last_sequence\": %u,\n", stats->last_seq);
    fprintf(fp, "  \"session_end_seen\": %s,\n",
            stats->saw_session_end ? "true" : "false");
    fprintf(fp, "  \"frame_size_min\": %" PRIu64 ",\n", stats->min_frame_size);
    fprintf(fp, "  \"frame_size_max\": %" PRIu64 ",\n", stats->max_frame_size);
    fprintf(fp, "  \"type_counts\": {\"final\": %" PRIu64
            ", \"raw_dbr\": %" PRIu64 ", \"rx_ppdu\": %" PRIu64
            ", \"dbr_meta\": %" PRIu64 ", \"session_start\": %" PRIu64
            ", \"session_end\": %" PRIu64 ", \"rearm\": %" PRIu64
            ", \"unknown\": %" PRIu64 "},\n",
            stats->type_final, stats->type_raw_dbr, stats->type_rx_ppdu,
            stats->type_dbr_meta, stats->type_session_start,
            stats->type_session_end, stats->type_rearm,
            stats->type_unknown);
    fprintf(fp, "  \"segments\": [\n");
    for (i = 0; i < out->segment_count; i++) {
        fprintf(fp, "    {\"path\": "); json_string(fp, out->segments[i].path);
        fprintf(fp, ", \"bytes\": %" PRIu64
                ", \"records\": %" PRIu64
                ", \"complete\": %s, \"sha256\": \"%s\"}%s\n",
                out->segments[i].bytes, out->segments[i].records,
                out->segments[i].complete ? "true" : "false",
                out->segments[i].sha256,
                i + 1U == out->segment_count ? "" : ",");
    }
    fprintf(fp, "  ],\n  \"trailing_buffer_bytes\": %" PRIu64
            ",\n  \"trailing_incomplete_frames\": %" PRIu64
            ",\n  \"unprocessed_buffer_bytes\": %" PRIu64
            ",\n  \"drain_records_discarded\": %" PRIu64 "\n}\n",
            stats->trailing_buffer_bytes, stats->trailing_incomplete_frames,
            stats->unprocessed_buffer_bytes,
            stats->drain_records_discarded);
    return fclose(fp);
}

static uint64_t read_u64_file(const char *path)
{
    char buf[64];
    char *end = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t len;
    unsigned long long value;
    if (fd < 0)
        return 0;
    len = read(fd, buf, sizeof(buf) - 1U);
    (void)close(fd);
    if (len <= 0)
        return 0;
    buf[len] = '\0';
    errno = 0;
    value = strtoull(buf, &end, 10);
    return errno || end == buf ? 0 : value;
}

static void usage(FILE *fp, const char *prog)
{
    fprintf(fp,
            "Usage: %s [options]\n"
            "  --input PATH --output PATH --metadata PATH --control PATH\n"
            "  --seconds N --max-bytes N --max-records N\n"
            "  --read-size N --max-frame-bytes N --max-resync-bytes N\n"
            "  --rotate-mb N --rotate-seconds N --stats-seconds N\n"
            "  --relay-reset --relay-on --relay-off-on-exit\n"
            "  --stop-on-session-end --follow --no-follow --no-metadata --verbose\n",
            prog);
}

static int parse_args(int argc, char **argv, struct options *opt)
{
    int i;
    memset(opt, 0, sizeof(*opt));
    opt->input = DEFAULT_INPUT;
    opt->output = DEFAULT_OUTPUT;
    opt->control = DEFAULT_CONTROL;
    opt->seconds = DEFAULT_SECONDS;
    opt->max_bytes = DEFAULT_MAX_MB * 1024ULL * 1024ULL;
    opt->read_size = DEFAULT_READ_SIZE;
    opt->max_frame_bytes = DEFAULT_MAX_FRAME;
    opt->max_resync_bytes = DEFAULT_MAX_RESYNC;
    opt->stats_seconds = DEFAULT_STATS_SECONDS;
    opt->follow = true;

    for (i = 1; i < argc; i++) {
        uint64_t value;
#define NEXT_U64(_field) \
        do { if (++i >= argc || !parse_u64(argv[i], &(_field))) return -1; } while (0)
        if (!strcmp(argv[i], "--input") && i + 1 < argc)
            opt->input = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            opt->output = argv[++i];
        else if (!strcmp(argv[i], "--metadata") && i + 1 < argc)
            opt->metadata = argv[++i];
        else if (!strcmp(argv[i], "--control") && i + 1 < argc)
            opt->control = argv[++i];
        else if (!strcmp(argv[i], "--seconds")) NEXT_U64(opt->seconds);
        else if (!strcmp(argv[i], "--max-bytes")) NEXT_U64(opt->max_bytes);
        else if (!strcmp(argv[i], "--max-records")) NEXT_U64(opt->max_records);
        else if (!strcmp(argv[i], "--max-frame-bytes")) NEXT_U64(opt->max_frame_bytes);
        else if (!strcmp(argv[i], "--max-resync-bytes")) NEXT_U64(opt->max_resync_bytes);
        else if (!strcmp(argv[i], "--rotate-seconds")) NEXT_U64(opt->rotate_seconds);
        else if (!strcmp(argv[i], "--stats-seconds")) NEXT_U64(opt->stats_seconds);
        else if (!strcmp(argv[i], "--rotate-mb")) {
            NEXT_U64(value);
            if (!checked_mul_u64(value, 1024ULL * 1024ULL,
                                 &opt->rotate_bytes))
                return -1;
        } else if (!strcmp(argv[i], "--max-mb")) {
            NEXT_U64(value);
            if (!checked_mul_u64(value, 1024ULL * 1024ULL,
                                 &opt->max_bytes))
                return -1;
        } else if (!strcmp(argv[i], "--read-size")) {
            NEXT_U64(value); if (!value || value > SIZE_MAX) return -1;
            opt->read_size = (size_t)value;
        } else if (!strcmp(argv[i], "--relay-reset")) opt->relay_reset = true;
        else if (!strcmp(argv[i], "--relay-on")) opt->relay_on = true;
        else if (!strcmp(argv[i], "--relay-off-on-exit")) opt->relay_off_on_exit = true;
        else if (!strcmp(argv[i], "--stop-on-session-end")) opt->stop_on_session_end = true;
        else if (!strcmp(argv[i], "--no-follow")) opt->follow = false;
        else if (!strcmp(argv[i], "--follow")) opt->follow = true;
        else if (!strcmp(argv[i], "--no-metadata")) opt->no_metadata = true;
        else if (!strcmp(argv[i], "--verbose")) opt->verbose = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout, argv[0]); exit(0);
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            return -1;
        }
#undef NEXT_U64
    }
    if (opt->max_frame_bytes < CFRR_V2_HDR_LEN ||
        opt->max_frame_bytes > SIZE_MAX - opt->read_size - CFRR_MAX_HDR_LEN)
        return -1;
    if (opt->metadata && !strcmp(opt->metadata, opt->output))
        return -1;
    return 0;
}

static int read_once(int fd, struct buffer *buf, uint8_t *read_buf,
                     const struct options *opt, struct output *out,
                     struct stats *stats, bool stop_for_session_end)
{
    ssize_t rd = read(fd, read_buf, opt->read_size);
    int processed;
    if (rd < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return 1;
        return -1;
    }
    if (!rd) {
        stats->eof_count++;
        return 2;
    }
    stats->read_calls++;
    stats->input_bytes += (uint64_t)rd;
    if (buffer_append(buf, read_buf, (size_t)rd) != 0)
        return -1;
    processed = process_buffer(buf, out, opt, stats, stop_for_session_end);
    return processed;
}

int main(int argc, char **argv)
{
    struct options opt;
    struct stats stats = {0};
    struct buffer buf = {0};
    struct output out = { .fd = -1 };
    uint8_t *read_buf = NULL;
    uint64_t start_ms, stop_deadline_ms = UINT64_MAX;
    uint64_t next_stats_ms = UINT64_MAX;
    uint64_t start_real_ns = now_real_ns(), end_real_ns;
    uint64_t kernel_drops_before = 0, kernel_drops_after;
    int in_fd = -1, exit_status = 1;
    bool relay_off_sent = false;

    if (parse_args(argc, argv, &opt) != 0) {
        usage(stderr, argv[0]);
        return 2;
    }
    out.opt = &opt;
    buf.max_cap = (size_t)opt.max_frame_bytes + opt.read_size +
                  CFRR_MAX_HDR_LEN;
    read_buf = malloc(opt.read_size);
    if (!read_buf) {
        perror("malloc");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (opt.relay_reset && relay_command(&opt, "relay_reset\n") != 0) {
        perror("relay_reset"); goto out;
    }
    in_fd = open(opt.input, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (in_fd < 0) {
        perror(opt.input); goto out;
    }
    if (output_open_segment(&out) != 0) {
        perror(opt.output); goto out;
    }
    if (opt.relay_on && relay_command(&opt, "relay_on\n") != 0) {
        perror("relay_on"); goto out;
    }

    start_ms = now_ms(CLOCK_MONOTONIC);
    start_real_ns = now_real_ns();
    kernel_drops_before = read_u64_file(
        "/sys/kernel/qca6490/cfr_records_dropped");
    if (opt.seconds)
        stop_deadline_ms = deadline_ms(start_ms, opt.seconds);
    if (opt.stats_seconds)
        next_stats_ms = deadline_ms(start_ms, opt.stats_seconds);

    while (!stop_requested) {
        struct pollfd pfd = { .fd = in_fd, .events = POLLIN };
        uint64_t now = now_ms(CLOCK_MONOTONIC);
        int timeout_ms = 250;
        int ret;

        if (now >= stop_deadline_ms) { stats.stop_reason = "seconds"; break; }
        if (opt.max_records && stats.records >= opt.max_records) {
            stats.stop_reason = "max_records"; break;
        }
        if (opt.max_bytes && stats.output_bytes >= opt.max_bytes) {
            stats.stop_reason = "max_bytes"; break;
        }
        if (now >= next_stats_ms) {
            fprintf(stderr, "records=%" PRIu64 " bytes=%" PRIu64
                    " gaps=%" PRIu64 " invalid=%" PRIu64
                    " resync=%" PRIu64 "\n", stats.records,
                    stats.output_bytes, stats.sequence_gaps,
                    stats.invalid_frames, stats.resync_bytes);
            next_stats_ms = deadline_ms(now, opt.stats_seconds);
        }
        if (stop_deadline_ms != UINT64_MAX &&
            stop_deadline_ms - now < (uint64_t)timeout_ms)
            timeout_ms = (int)(stop_deadline_ms - now);
        ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll"); goto out;
        }
        if (!ret) continue;
        if (pfd.revents & POLLNVAL) {
            errno = EBADF; perror("poll input"); goto out;
        }
        if ((pfd.revents & POLLERR) && !(pfd.revents & POLLIN)) {
            errno = EIO; perror("poll input"); goto out;
        }
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & POLLHUP) {
                stats.stop_reason = "input_hup";
                break;
            }
            continue;
        }
        ret = read_once(in_fd, &buf, read_buf, &opt, &out, &stats, false);
        if (ret < 0) { perror("read/process"); goto out; }
        if (ret == 2) {
            if (!opt.follow || (pfd.revents & POLLHUP)) {
                stats.stop_reason = "eof";
                break;
            }
            (void)poll(NULL, 0, 10);
            continue;
        }
        if (stats.stop_reason) break;
    }
    if (stop_requested)
        stats.stop_reason = "signal";

    if (opt.relay_off_on_exit) {
        uint64_t drain_deadline = now_ms(CLOCK_MONOTONIC) + 1000ULL;
        int drain_status = 1;

        if (relay_command(&opt, "relay_off\n") != 0) {
            perror("relay_off"); goto out;
        }
        relay_off_sent = true;
        if (buffer_len(&buf)) {
            drain_status = process_buffer(&buf, &out, &opt, &stats, true);
            if (drain_status < 0) {
                perror("drain process"); goto out;
            }
        }
        while (drain_status > 0 && !stats.saw_session_end &&
               now_ms(CLOCK_MONOTONIC) < drain_deadline) {
            struct pollfd pfd = { .fd = in_fd, .events = POLLIN };
            int ret = poll(&pfd, 1, 100);
            if (ret < 0 && errno != EINTR) { perror("drain poll"); goto out; }
            if (ret > 0 && (pfd.revents & POLLNVAL)) {
                errno = EBADF; perror("drain poll"); goto out;
            }
            if (ret > 0 && (pfd.revents & POLLERR) &&
                !(pfd.revents & POLLIN)) {
                errno = EIO; perror("drain poll"); goto out;
            }
            if (ret > 0) {
                if (!(pfd.revents & POLLIN))
                    break;
                ret = read_once(in_fd, &buf, read_buf, &opt, &out, &stats, true);
                if (ret < 0) { perror("drain read"); goto out; }
                if (!ret || ret == 2) {
                    drain_status = ret;
                    break;
                }
            }
        }
    }

    if (buffer_len(&buf)) {
        stats.trailing_buffer_bytes = buffer_len(&buf);
        stats.unprocessed_buffer_bytes = buffer_len(&buf);
        if (buffer_len(&buf) < CFRR_V1_HDR_LEN ||
            get_le32(buf.data + buf.start) != CFRR_MAGIC) {
            stats.trailing_incomplete_frames = 1;
        } else {
            uint16_t hdr_len = get_le16(buf.data + buf.start + 6U);
            uint32_t payload_len = get_le32(buf.data + buf.start + 20U);
            uint64_t declared = (uint64_t)hdr_len + payload_len;

            if (hdr_len < CFRR_V1_HDR_LEN || declared > buffer_len(&buf))
                stats.trailing_incomplete_frames = 1;
        }
    }
    if (!stats.stop_reason)
        stats.stop_reason = "completed";
    if (output_close_segment(&out) != 0) {
        perror("close output"); goto out;
    }
    (void)report_reader_stats(&opt, &stats);
    exit_status = 0;

out:
    if (opt.relay_off_on_exit && !relay_off_sent) {
        if (relay_command(&opt, "relay_off\n") != 0)
            perror("relay_off cleanup");
        else
            relay_off_sent = true;
    }
    if (out.fd >= 0 && output_close_segment(&out) != 0 && exit_status == 0)
        exit_status = 1;
    if (in_fd >= 0)
        (void)close(in_fd);
    end_real_ns = now_real_ns();
    kernel_drops_after = read_u64_file(
        "/sys/kernel/qca6490/cfr_records_dropped");
    if (write_metadata(&opt, &stats, &out, start_real_ns, end_real_ns,
                       exit_status, kernel_drops_before,
                       kernel_drops_after) != 0) {
        perror("write metadata");
        exit_status = 1;
    }
    fprintf(stderr, "records=%" PRIu64 " bytes=%" PRIu64
            " sessions=%" PRIu64 " gaps=%" PRIu64
            " invalid=%" PRIu64 " resync=%" PRIu64 " stop=%s\n",
            stats.records, stats.output_bytes, stats.sessions_seen,
            stats.sequence_gaps, stats.invalid_frames, stats.resync_bytes,
            stats.stop_reason ? stats.stop_reason : "error");
    free(buf.data);
    free(read_buf);
    return exit_status;
}
