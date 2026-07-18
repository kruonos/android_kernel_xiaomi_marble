// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <inttypes.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NL80211_CMD_VENDOR 103

#define NL80211_ATTR_IFINDEX 3
#define NL80211_ATTR_VENDOR_ID 195
#define NL80211_ATTR_VENDOR_SUBCMD 196
#define NL80211_ATTR_VENDOR_DATA 197

#define QCA_NL80211_VENDOR_ID 0x001374U
#define QCA_NL80211_VENDOR_SUBCMD_PEER_CFR_CAPTURE_CFG 173U

enum qca_wlan_vendor_cfr_data_transport_modes {
    QCA_WLAN_VENDOR_CFR_DATA_RELAY_FS = 0,
    QCA_WLAN_VENDOR_CFR_DATA_NETLINK_EVENTS = 1,
};

enum qca_wlan_vendor_cfr_capture_type {
    QCA_WLAN_VENDOR_CFR_DIRECT_FTM = 0,
    QCA_WLAN_VENDOR_CFR_ALL_FTM_ACK = 1,
    QCA_WLAN_VENDOR_CFR_DIRECT_NDPA_NDP = 2,
    QCA_WLAN_VENDOR_CFR_TA_RA = 3,
    QCA_WLAN_VENDOR_CFR_ALL_PACKET = 4,
    QCA_WLAN_VENDOR_CFR_NDPA_NDP_ALL = 5,
};

enum qca_wlan_vendor_peer_cfr_capture_attr {
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_CAPTURE_INVALID = 0,
    QCA_WLAN_VENDOR_ATTR_CFR_PEER_MAC_ADDR = 1,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE = 2,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_BANDWIDTH = 3,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_PERIODICITY = 4,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_METHOD = 5,
    QCA_WLAN_VENDOR_ATTR_PERIODIC_CFR_CAPTURE_ENABLE = 6,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_VERSION = 7,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE_GROUP_BITMAP = 8,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_DURATION = 9,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_INTERVAL = 10,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_CAPTURE_TYPE = 11,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_UL_MU_MASK = 12,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_FREEZE_TLV_DELAY_COUNT = 13,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TABLE = 14,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_ENTRY = 15,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_NUMBER = 16,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TA = 17,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_RA = 18,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TA_MASK = 19,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_RA_MASK = 20,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_NSS = 21,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_BW = 22,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_MGMT_FILTER = 23,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_CTRL_FILTER = 24,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_DATA_FILTER = 25,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_TRANSPORT_MODE = 26,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_RECEIVER_PID = 27,
    QCA_WLAN_VENDOR_ATTR_PEER_CFR_RESP_DATA = 28,
};

#define GENL_ID_CTRL NLMSG_MIN_TYPE
#define ENHANCED_CFR_VERSION 2U
#define MAX_CMD_MSG_SIZE 8192U
#define MAX_EVENT_MSG_SIZE (256U * 1024U)
#define NL_RCVBUF_SIZE (1024U * 1024U)
#define CFR_PREVIEW_LEN 96U

static size_t nl_align(size_t len)
{
    return (len + 3U) & ~3U;
}

struct nl_builder {
    uint8_t buf[MAX_CMD_MSG_SIZE];
    size_t len;
};

static void *builder_put(struct nl_builder *b, size_t len)
{
    void *ptr;

    if (b->len + len > sizeof(b->buf)) {
        fprintf(stderr, "message too large\n");
        exit(1);
    }
    ptr = &b->buf[b->len];
    memset(ptr, 0, len);
    b->len += len;
    return ptr;
}

static struct nlattr *nla_put_raw(struct nl_builder *b, uint16_t type,
                                  const void *data, uint16_t data_len)
{
    const size_t total = nl_align(sizeof(struct nlattr) + data_len);
    struct nlattr *nla = builder_put(b, total);

    nla->nla_type = type;
    nla->nla_len = (uint16_t)(sizeof(struct nlattr) + data_len);
    if (data_len != 0U && data != NULL) {
        memcpy((uint8_t *)nla + sizeof(*nla), data, data_len);
    }
    return nla;
}

static struct nlattr *nla_start_nested(struct nl_builder *b, uint16_t type)
{
    return nla_put_raw(b, type, NULL, 0);
}

static void nla_end_nested(struct nl_builder *b, struct nlattr *nla)
{
    const uintptr_t start = (uintptr_t)b->buf;
    const uintptr_t here = (uintptr_t)nla;
    nla->nla_len = (uint16_t)(b->len - (size_t)(here - start));
}

static void nla_put_u8(struct nl_builder *b, uint16_t type, uint8_t value)
{
    nla_put_raw(b, type, &value, sizeof(value));
}

static void nla_put_u32(struct nl_builder *b, uint16_t type, uint32_t value)
{
    nla_put_raw(b, type, &value, sizeof(value));
}

static void nla_put_string(struct nl_builder *b, uint16_t type, const char *value)
{
    nla_put_raw(b, type, value, (uint16_t)(strlen(value) + 1U));
}

static void nla_put_flag(struct nl_builder *b, uint16_t type)
{
    nla_put_raw(b, type, NULL, 0);
}

static int nl_open(uint32_t *portid)
{
    int fd;
    int rcvbuf = (int)NL_RCVBUF_SIZE;
    struct sockaddr_nl addr;
    socklen_t addrlen = sizeof(addr);

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) {
        perror("socket(AF_NETLINK)");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("setsockopt(SO_RCVBUF)");
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0;
    addr.nl_groups = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(netlink)");
        close(fd);
        return -1;
    }

    if (getsockname(fd, (struct sockaddr *)&addr, &addrlen) < 0) {
        perror("getsockname(netlink)");
        close(fd);
        return -1;
    }

    *portid = addr.nl_pid;
    return fd;
}

static int nl_send_builder(int fd, struct nl_builder *b)
{
    struct sockaddr_nl addr;
    ssize_t written;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    written = sendto(fd, b->buf, b->len, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (written < 0) {
        perror("sendto(netlink)");
        return -1;
    }
    if ((size_t)written != b->len) {
        fprintf(stderr, "short netlink send: %zd/%zu\n", written, b->len);
        return -1;
    }
    return 0;
}

static int parse_ack_or_error(const struct nlmsghdr *nlh)
{
    const struct nlmsgerr *err;

    if (nlh->nlmsg_type != NLMSG_ERROR) {
        return 1;
    }
    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*err))) {
        fprintf(stderr, "short NLMSG_ERROR\n");
        return -1;
    }
    err = NLMSG_DATA(nlh);
    if (err->error == 0) {
        return 0;
    }
    errno = -err->error;
    perror("netlink ack");
    return -1;
}

static int recv_one(int fd, uint16_t expect_type, uint32_t expect_seq,
                    uint32_t *family_id)
{
    uint8_t buf[MAX_CMD_MSG_SIZE];
    struct nlmsghdr *nlh;
    ssize_t got;

    got = recv(fd, buf, sizeof(buf), 0);
    if (got < 0) {
        perror("recv(netlink)");
        return -1;
    }

    for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, (unsigned int)got);
         nlh = NLMSG_NEXT(nlh, got)) {
        if (nlh->nlmsg_seq != expect_seq) {
            continue;
        }
        if (nlh->nlmsg_type == NLMSG_DONE) {
            return 0;
        }
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            return parse_ack_or_error(nlh);
        }
        if (nlh->nlmsg_type == expect_type && family_id != NULL) {
            const struct genlmsghdr *gh = NLMSG_DATA(nlh);
            int rem = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*gh));
            struct nlattr *attr = (struct nlattr *)((uint8_t *)gh + sizeof(*gh));

            while (rem >= (int)sizeof(*attr) && attr->nla_len >= sizeof(*attr) && attr->nla_len <= rem) {
                const uint16_t alen = attr->nla_len - (uint16_t)sizeof(*attr);
                if (attr->nla_type == CTRL_ATTR_FAMILY_ID && alen >= sizeof(uint16_t)) {
                    uint16_t fid;
                    memcpy(&fid, (uint8_t *)attr + sizeof(*attr), sizeof(fid));
                    *family_id = fid;
                    return 0;
                }
                rem -= (int)nl_align(attr->nla_len);
                attr = (struct nlattr *)((uint8_t *)attr + nl_align(attr->nla_len));
            }
        }
    }

    return 1;
}

static int parse_capture_type(const char *value, uint8_t *capture_type)
{
    char *end = NULL;
    unsigned long numeric;

    if (strcmp(value, "direct_ftm") == 0 || strcmp(value, "ftm") == 0) {
        *capture_type = QCA_WLAN_VENDOR_CFR_DIRECT_FTM;
        return 0;
    }
    if (strcmp(value, "all_ftm_ack") == 0 || strcmp(value, "ack") == 0) {
        *capture_type = QCA_WLAN_VENDOR_CFR_ALL_FTM_ACK;
        return 0;
    }
    if (strcmp(value, "direct_ndpa") == 0 || strcmp(value, "ndpa") == 0) {
        *capture_type = QCA_WLAN_VENDOR_CFR_DIRECT_NDPA_NDP;
        return 0;
    }
    if (strcmp(value, "ta_ra") == 0) {
        *capture_type = QCA_WLAN_VENDOR_CFR_TA_RA;
        return 0;
    }
    if (strcmp(value, "all_packet") == 0 || strcmp(value, "all") == 0) {
        *capture_type = QCA_WLAN_VENDOR_CFR_ALL_PACKET;
        return 0;
    }
    if (strcmp(value, "all_ndpa") == 0 || strcmp(value, "ndpa_all") == 0) {
        *capture_type = QCA_WLAN_VENDOR_CFR_NDPA_NDP_ALL;
        return 0;
    }

    errno = 0;
    numeric = strtoul(value, &end, 0);
    if (errno == 0 && end != value && *end == '\0' &&
        numeric <= QCA_WLAN_VENDOR_CFR_NDPA_NDP_ALL) {
        *capture_type = (uint8_t)numeric;
        return 0;
    }

    fprintf(stderr, "invalid capture type '%s'\n", value);
    fprintf(stderr, "valid: direct_ftm, all_ftm_ack, direct_ndpa, all_ndpa, ta_ra, all_packet, or 0-5\n");
    return -1;
}

static int resolve_nl80211_family(int fd, uint32_t portid, uint32_t *family_id)
{
    struct nl_builder b = {0};
    struct nlmsghdr *nlh;
    struct genlmsghdr *gh;
    const uint32_t seq = 1;

    nlh = builder_put(&b, NLMSG_HDRLEN + GENL_HDRLEN);
    nlh->nlmsg_len = b.len;
    nlh->nlmsg_type = GENL_ID_CTRL;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_pid = portid;

    gh = (struct genlmsghdr *)((uint8_t *)nlh + NLMSG_HDRLEN);
    gh->cmd = CTRL_CMD_GETFAMILY;
    gh->version = 1;

    nla_put_string(&b, CTRL_ATTR_FAMILY_NAME, "nl80211");
    nlh->nlmsg_len = b.len;

    if (nl_send_builder(fd, &b) < 0) {
        return -1;
    }

    for (;;) {
        int rc = recv_one(fd, GENL_ID_CTRL, seq, family_id);
        if (rc <= 0) {
            return rc;
        }
    }
}

static int send_cfr_cmd(int fd, uint32_t portid, uint16_t nl80211_family,
                        int ifindex, bool enable, uint8_t capture_type)
{
    struct nl_builder b = {0};
    struct nlmsghdr *nlh;
    struct genlmsghdr *gh;
    struct nlattr *vendor_data;
    const uint32_t seq = 2 + (enable ? 1U : 0U);

    nlh = builder_put(&b, NLMSG_HDRLEN + GENL_HDRLEN);
    nlh->nlmsg_len = b.len;
    nlh->nlmsg_type = nl80211_family;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_pid = portid;

    gh = (struct genlmsghdr *)((uint8_t *)nlh + NLMSG_HDRLEN);
    gh->cmd = NL80211_CMD_VENDOR;
    gh->version = 1;

    nla_put_u32(&b, NL80211_ATTR_IFINDEX, (uint32_t)ifindex);
    nla_put_u32(&b, NL80211_ATTR_VENDOR_ID, QCA_NL80211_VENDOR_ID);
    nla_put_u32(&b, NL80211_ATTR_VENDOR_SUBCMD,
                QCA_NL80211_VENDOR_SUBCMD_PEER_CFR_CAPTURE_CFG);

    vendor_data = nla_start_nested(&b, NL80211_ATTR_VENDOR_DATA);
    if (enable) {
        nla_put_flag(&b, QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE);
    }
    nla_put_u8(&b, QCA_WLAN_VENDOR_ATTR_PEER_CFR_VERSION, ENHANCED_CFR_VERSION);
    if (enable) {
        nla_put_u32(&b, QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE_GROUP_BITMAP, 1U);
        nla_put_u32(&b, QCA_WLAN_VENDOR_ATTR_PEER_CFR_CAPTURE_TYPE,
                    (uint32_t)capture_type);
        nla_put_u8(&b, QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_TRANSPORT_MODE,
                   QCA_WLAN_VENDOR_CFR_DATA_NETLINK_EVENTS);
        nla_put_u32(&b, QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_RECEIVER_PID, portid);
    }
    nla_end_nested(&b, vendor_data);
    nlh->nlmsg_len = b.len;

    if (nl_send_builder(fd, &b) < 0) {
        return -1;
    }

    for (;;) {
        int rc = recv_one(fd, nl80211_family, seq, NULL);
        if (rc <= 0) {
            return rc;
        }
    }
}

static void hexdump(const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        printf("%02x", data[i]);
        if ((i + 1U) != len) {
            putchar(' ');
        }
    }
    putchar('\n');
}

static int listen_for_events(int fd, uint16_t nl80211_family, int timeout_ms)
{
    uint8_t *buf;
    int64_t end_ms;
    unsigned int event_count = 0;

    buf = malloc(MAX_EVENT_MSG_SIZE);
    if (!buf) {
        perror("malloc(event buffer)");
        return -1;
    }

    end_ms = (int64_t)timeout_ms;
    if (timeout_ms >= 0) {
        struct timespec ts;

        if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
            perror("clock_gettime");
            free(buf);
            return -1;
        }
        end_ms += (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int remaining = timeout_ms;
        int rc;

        if (timeout_ms >= 0) {
            struct timespec ts;
            int64_t now_ms;

            if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
                perror("clock_gettime");
                free(buf);
                return -1;
            }
            now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            remaining = (int)(end_ms - now_ms);
            if (remaining <= 0) {
                printf("CFR events received=%u\n", event_count);
                free(buf);
                return 0;
            }
        }

        rc = poll(&pfd, 1, remaining);
        if (rc < 0) {
            perror("poll");
            free(buf);
            return -1;
        }
        if (rc == 0) {
            if (event_count == 0) {
                printf("no CFR event within %d ms\n", timeout_ms);
            } else {
                printf("CFR events received=%u\n", event_count);
            }
            free(buf);
            return 0;
        }
        if ((pfd.revents & POLLIN) == 0) {
            printf("unexpected poll revents: 0x%x\n", pfd.revents);
            free(buf);
            return 0;
        }

        for (;;) {
            ssize_t got = recv(fd, buf, MAX_EVENT_MSG_SIZE, MSG_DONTWAIT);
            struct nlmsghdr *nlh;

            if (got < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                perror("recv(event)");
                free(buf);
                return -1;
            }

            for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, (unsigned int)got);
                 nlh = NLMSG_NEXT(nlh, got)) {
                if (nlh->nlmsg_type != nl80211_family) {
                    if (nlh->nlmsg_type == NLMSG_ERROR) {
                        parse_ack_or_error(nlh);
                    }
                    continue;
                }
                if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct genlmsghdr))) {
                    continue;
                }

                const struct genlmsghdr *gh = NLMSG_DATA(nlh);
                int rem = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*gh));
                struct nlattr *attr = (struct nlattr *)((uint8_t *)gh + sizeof(*gh));

                event_count++;
                printf("nl80211 vendor event #%u cmd=%u len=%u\n",
                       event_count, gh->cmd, nlh->nlmsg_len);
                while (rem >= (int)sizeof(*attr) && attr->nla_len >= sizeof(*attr) && attr->nla_len <= rem) {
                    const uint16_t type = attr->nla_type;
                    const uint16_t alen = attr->nla_len - (uint16_t)sizeof(*attr);
                    const uint8_t *adata = (const uint8_t *)attr + sizeof(*attr);

                    if (type == NL80211_ATTR_VENDOR_DATA) {
                        int vrem = alen;
                        struct nlattr *vattr = (struct nlattr *)adata;
                        while (vrem >= (int)sizeof(*vattr) && vattr->nla_len >= sizeof(*vattr) && vattr->nla_len <= vrem) {
                            uint16_t vtype = vattr->nla_type;
                            uint16_t vlen = vattr->nla_len - (uint16_t)sizeof(*vattr);
                            const uint8_t *vdata = (const uint8_t *)vattr + sizeof(*vattr);
                            printf("  vendor attr %u len=%u", vtype, vlen);
                            if (vtype == QCA_WLAN_VENDOR_ATTR_PEER_CFR_RESP_DATA && vlen > 0U) {
                                printf(" preview_len=%u data=",
                                       vlen > CFR_PREVIEW_LEN ? CFR_PREVIEW_LEN : vlen);
                                hexdump(vdata, vlen > CFR_PREVIEW_LEN ? CFR_PREVIEW_LEN : vlen);
                            } else {
                                printf("\n");
                            }
                            vrem -= (int)nl_align(vattr->nla_len);
                            vattr = (struct nlattr *)((uint8_t *)vattr + nl_align(vattr->nla_len));
                        }
                    }

                    rem -= (int)nl_align(attr->nla_len);
                    attr = (struct nlattr *)((uint8_t *)attr + nl_align(attr->nla_len));
                }
            }
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage:\n"
            "  %s family\n"
            "  %s start [ifname] [capture_type]\n"
            "  %s stop [ifname]\n"
            "  %s probe [ifname] [listen_ms] [capture_type]\n"
            "capture_type: direct_ftm, all_ftm_ack, direct_ndpa, all_ndpa, ta_ra, all_packet, or 0-5\n",
            prog, prog, prog, prog);
}

static void print_selinux_context(void)
{
    FILE *fp = fopen("/proc/self/attr/current", "r");
    char buf[256];

    if (!fp) {
        return;
    }
    if (fgets(buf, sizeof(buf), fp) != NULL) {
        size_t len = strlen(buf);
        while (len > 0U && (buf[len - 1U] == '\n' || buf[len - 1U] == '\0')) {
            buf[len - 1U] = '\0';
            --len;
        }
        printf("selinux=%s\n", buf);
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    const char *cmd;
    const char *ifname = "wlan0";
    uint32_t portid = 0;
    uint32_t family = 0;
    int fd;
    int ifindex;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    cmd = argv[1];
    if (argc >= 3) {
        ifname = argv[2];
    }

    print_selinux_context();

    fd = nl_open(&portid);
    if (fd < 0) {
        return 1;
    }
    printf("netlink pid=%" PRIu32 "\n", portid);

    if (resolve_nl80211_family(fd, portid, &family) < 0) {
        close(fd);
        return 1;
    }
    printf("nl80211 family=%" PRIu32 "\n", family);

    if (strcmp(cmd, "family") == 0) {
        close(fd);
        return 0;
    }

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "if_nametoindex(%s) failed: %s\n", ifname, strerror(errno));
        close(fd);
        return 1;
    }
    printf("ifname=%s ifindex=%d\n", ifname, ifindex);

    if (strcmp(cmd, "start") == 0) {
        uint8_t capture_type = QCA_WLAN_VENDOR_CFR_DIRECT_FTM;
        if (argc >= 4 && parse_capture_type(argv[3], &capture_type) < 0) {
            close(fd);
            return 2;
        }
        printf("capture_type=%u\n", capture_type);
        if (send_cfr_cmd(fd, portid, (uint16_t)family, ifindex, true,
                         capture_type) < 0) {
            close(fd);
            return 1;
        }
        printf("CFR start command acknowledged\n");
        close(fd);
        return 0;
    }

    if (strcmp(cmd, "stop") == 0) {
        if (send_cfr_cmd(fd, portid, (uint16_t)family, ifindex, false,
                         QCA_WLAN_VENDOR_CFR_DIRECT_FTM) < 0) {
            close(fd);
            return 1;
        }
        printf("CFR stop command acknowledged\n");
        close(fd);
        return 0;
    }

    if (strcmp(cmd, "probe") == 0) {
        int listen_ms = 2000;
        uint8_t capture_type = QCA_WLAN_VENDOR_CFR_DIRECT_FTM;
        if (argc >= 4) {
            char *end = NULL;
            long parsed_ms;

            errno = 0;
            parsed_ms = strtol(argv[3], &end, 0);
            if (errno == 0 && end != argv[3] && *end == '\0') {
                listen_ms = (int)parsed_ms;
                if (listen_ms < 0) {
                    fprintf(stderr, "invalid listen_ms\n");
                    close(fd);
                    return 2;
                }
                if (argc >= 5 && parse_capture_type(argv[4], &capture_type) < 0) {
                    close(fd);
                    return 2;
                }
            } else if (parse_capture_type(argv[3], &capture_type) < 0) {
                close(fd);
                return 2;
            }
        }
        printf("capture_type=%u\n", capture_type);
        if (send_cfr_cmd(fd, portid, (uint16_t)family, ifindex, true,
                         capture_type) < 0) {
            close(fd);
            return 1;
        }
        printf("CFR start command acknowledged, listening %d ms\n", listen_ms);
        if (listen_for_events(fd, (uint16_t)family, listen_ms) < 0) {
            (void)send_cfr_cmd(fd, portid, (uint16_t)family, ifindex, false,
                               QCA_WLAN_VENDOR_CFR_DIRECT_FTM);
            close(fd);
            return 1;
        }
        if (send_cfr_cmd(fd, portid, (uint16_t)family, ifindex, false,
                         QCA_WLAN_VENDOR_CFR_DIRECT_FTM) < 0) {
            close(fd);
            return 1;
        }
        printf("CFR stop command acknowledged\n");
        close(fd);
        return 0;
    }

    usage(argv[0]);
    close(fd);
    return 2;
}
