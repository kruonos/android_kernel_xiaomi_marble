#define _GNU_SOURCE
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef NLA_ALIGNTO
#define NLA_ALIGNTO 4
#endif
#ifndef NLA_ALIGN
#define NLA_ALIGN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#endif
#ifndef NLA_HDRLEN
#define NLA_HDRLEN ((int)NLA_ALIGN(sizeof(struct nlattr)))
#endif
#define NLA_DATA(a) ((void *)((char *)(a) + NLA_HDRLEN))
#define NLA_PAYLOAD(a) ((int)((a)->nla_len - NLA_HDRLEN))
#define NLA_OK(a, len) ((len) >= (int)sizeof(struct nlattr) && \
                        (a)->nla_len >= sizeof(struct nlattr) && \
                        (a)->nla_len <= (len))
#define NLA_NEXT(a, len) ((len) -= NLA_ALIGN((a)->nla_len), \
                          (struct nlattr *)((char *)(a) + NLA_ALIGN((a)->nla_len)))
#define NLMSG_TAIL(n) ((struct nlattr *)((char *)(n) + NLMSG_ALIGN((n)->nlmsg_len)))

static int seqno = 200;

static void addattr(struct nlmsghdr *n, int maxlen, int type,
                    const void *data, int data_len)
{
    int len = NLA_HDRLEN + data_len;
    struct nlattr *a;
    if ((int)NLMSG_ALIGN(n->nlmsg_len) + NLA_ALIGN(len) > maxlen) {
        fprintf(stderr, "attribute overflow\n");
        exit(2);
    }
    a = NLMSG_TAIL(n);
    a->nla_type = type;
    a->nla_len = len;
    if (data_len)
        memcpy(NLA_DATA(a), data, data_len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + NLA_ALIGN(len);
}

static int send_genl(int fd, uint16_t family, uint8_t cmd, int flags,
                     void (*fill)(struct nlmsghdr *, int, void *), void *arg)
{
    unsigned char buf[4096] = {0};
    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    struct genlmsghdr *g;
    struct sockaddr_nl dst = {.nl_family = AF_NETLINK};
    n->nlmsg_len = NLMSG_LENGTH(sizeof(*g));
    n->nlmsg_type = family;
    n->nlmsg_flags = NLM_F_REQUEST | flags;
    n->nlmsg_seq = ++seqno;
    n->nlmsg_pid = getpid();
    g = NLMSG_DATA(n);
    g->cmd = cmd;
    if (fill)
        fill(n, sizeof(buf), arg);
    if (sendto(fd, n, n->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
        return -1;
    return n->nlmsg_seq;
}

static int recv_result(int fd, int seq, int *family)
{
    unsigned char buf[8192];
    for (;;) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0)
            return -1;
        for (struct nlmsghdr *n = (struct nlmsghdr *)buf;
             NLMSG_OK(n, (unsigned int)len); n = NLMSG_NEXT(n, len)) {
            if ((int)n->nlmsg_seq != seq)
                continue;
            if (n->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = NLMSG_DATA(n);
                if (!e->error)
                    return 0;
                errno = -e->error;
                return -1;
            }
            if (family) {
                struct genlmsghdr *g = NLMSG_DATA(n);
                int rem = n->nlmsg_len - NLMSG_LENGTH(sizeof(*g));
                for (struct nlattr *a = (struct nlattr *)((char *)g + GENL_HDRLEN);
                     NLA_OK(a, rem); a = NLA_NEXT(a, rem)) {
                    if (a->nla_type == CTRL_ATTR_FAMILY_ID && NLA_PAYLOAD(a) >= 2) {
                        *family = *(uint16_t *)NLA_DATA(a);
                        return 0;
                    }
                }
            }
        }
    }
}

static void fill_family(struct nlmsghdr *n, int maxlen, void *arg)
{
    const char name[] = "nl80211";
    (void)arg;
    addattr(n, maxlen, CTRL_ATTR_FAMILY_NAME, name, sizeof(name));
}

struct tx_args {
    uint32_t ifindex;
    uint32_t freq;
    unsigned char *frame;
    size_t frame_len;
    int wait_for_status;
};

static void fill_tx(struct nlmsghdr *n, int maxlen, void *arg)
{
    struct tx_args *a = arg;
    addattr(n, maxlen, NL80211_ATTR_IFINDEX, &a->ifindex, sizeof(a->ifindex));
    addattr(n, maxlen, NL80211_ATTR_WIPHY_FREQ, &a->freq, sizeof(a->freq));
    addattr(n, maxlen, NL80211_ATTR_FRAME, a->frame, a->frame_len);
    if (!a->wait_for_status)
        addattr(n, maxlen, NL80211_ATTR_DONT_WAIT_FOR_ACK, NULL, 0);
}

static int recv_tx_status(int fd, int family)
{
    struct timeval timeout = {.tv_sec = 5};
    unsigned char buf[8192];

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    for (;;) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0) {
            fprintf(stderr, "TX status timeout: errno=%d (%s)\n", errno,
                    strerror(errno));
            return -1;
        }
        for (struct nlmsghdr *n = (struct nlmsghdr *)buf;
             NLMSG_OK(n, (unsigned int)len); n = NLMSG_NEXT(n, len)) {
            struct genlmsghdr *g;
            struct nlattr *a;
            uint64_t cookie = 0;
            int ack = 0, rem;

            if (n->nlmsg_type != family)
                continue;
            g = NLMSG_DATA(n);
            if (g->cmd != NL80211_CMD_FRAME_TX_STATUS)
                continue;
            rem = n->nlmsg_len - NLMSG_LENGTH(sizeof(*g));
            for (a = (struct nlattr *)((char *)g + GENL_HDRLEN);
                 NLA_OK(a, rem); a = NLA_NEXT(a, rem)) {
                int type = a->nla_type & NLA_TYPE_MASK;

                if (type == NL80211_ATTR_COOKIE && NLA_PAYLOAD(a) >= 8)
                    memcpy(&cookie, NLA_DATA(a), sizeof(cookie));
                else if (type == NL80211_ATTR_ACK)
                    ack = 1;
            }
            printf("NL80211_CMD_FRAME_TX_STATUS cookie=%llu ack=%s\n",
                   (unsigned long long)cookie, ack ? "yes" : "no");
            return 0;
        }
    }
}

static int parse_mac(const char *text, unsigned char *mac)
{
    unsigned int v[6];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2],
               &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 255)
            return -1;
        mac[i] = v[i];
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char frame[96] = {
        0x40, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0, 0, 0, 0, 0, 0,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00,
    };
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    struct tx_args tx = {0};
    int fd, family = -1, seq;
    size_t ssid_len;

    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s <iface> <source-mac> <freq-mhz> <ssid> [wait]\n", argv[0]);
        return 2;
    }
    tx.ifindex = if_nametoindex(argv[1]);
    tx.freq = strtoul(argv[3], NULL, 10);
    ssid_len = strlen(argv[4]);
    if (!tx.ifindex || parse_mac(argv[2], &frame[10]) || ssid_len > 32) {
        fprintf(stderr, "invalid interface, MAC, or SSID\n");
        return 3;
    }
    frame[24] = 0;
    frame[25] = ssid_len;
    memcpy(&frame[26], argv[4], ssid_len);
    tx.frame = frame;
    tx.frame_len = 26 + ssid_len;
    tx.wait_for_status = argc == 6 && strcmp(argv[5], "wait") == 0;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("netlink socket");
        return 4;
    }
    seq = send_genl(fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0, fill_family, NULL);
    if (seq < 0 || recv_result(fd, seq, &family) < 0) {
        perror("resolve nl80211");
        return 5;
    }
    seq = send_genl(fd, family, NL80211_CMD_FRAME, NLM_F_ACK, fill_tx, &tx);
    if (seq < 0 || recv_result(fd, seq, NULL) < 0) {
        fprintf(stderr, "NL80211_CMD_FRAME failed: errno=%d (%s)\n", errno, strerror(errno));
        return 6;
    }
    printf("NL80211_CMD_FRAME accepted ifindex=%u freq=%u frame_bytes=%zu\n",
           tx.ifindex, tx.freq, tx.frame_len);
    if (tx.wait_for_status && recv_tx_status(fd, family) < 0)
        return 7;
    close(fd);
    return 0;
}
