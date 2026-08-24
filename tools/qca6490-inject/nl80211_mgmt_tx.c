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

#define MAX_FRAME 2048

static int seqno = 300;

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
};

static void fill_tx(struct nlmsghdr *n, int maxlen, void *arg)
{
    struct tx_args *a = arg;
    addattr(n, maxlen, NL80211_ATTR_IFINDEX, &a->ifindex, sizeof(a->ifindex));
    addattr(n, maxlen, NL80211_ATTR_WIPHY_FREQ, &a->freq, sizeof(a->freq));
    addattr(n, maxlen, NL80211_ATTR_FRAME, a->frame, a->frame_len);
    addattr(n, maxlen, NL80211_ATTR_DONT_WAIT_FOR_ACK, NULL, 0);
}

static int parse_hex(const char *text, unsigned char *out, size_t max)
{
    size_t len = strlen(text);
    size_t i;
    if (len % 2 || len / 2 > max)
        return -1;
    for (i = 0; i < len; i += 2) {
        unsigned int v;
        if (sscanf(text + i, "%2x", &v) != 1)
            return -1;
        out[i / 2] = (unsigned char)v;
    }
    return (int)(len / 2);
}

int main(int argc, char **argv)
{
    unsigned char frame[MAX_FRAME];
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    struct tx_args tx = {0};
    FILE *fp;
    size_t got;
    int fd, family = -1, seq;

    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <iface> <freq-mhz> <frame-file-or-hex>\n",
                argv[0]);
        return 2;
    }
    tx.ifindex = if_nametoindex(argv[1]);
    tx.freq = strtoul(argv[2], NULL, 10);
    if (!tx.ifindex) {
        fprintf(stderr, "invalid interface\n");
        return 3;
    }
    fp = fopen(argv[3], "rb");
    if (fp) {
        got = fread(frame, 1, sizeof(frame), fp);
        fclose(fp);
        if (got < 10 || got > sizeof(frame)) {
            fprintf(stderr, "frame file must be 10..%u bytes\n", MAX_FRAME);
            return 4;
        }
        tx.frame_len = got;
    } else {
        int n = parse_hex(argv[3], frame, sizeof(frame));
        if (n < 0) {
            fprintf(stderr, "cannot read frame file and input is not hex\n");
            return 4;
        }
        tx.frame_len = (size_t)n;
    }
    tx.frame = frame;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("netlink socket");
        return 5;
    }
    seq = send_genl(fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0, fill_family, NULL);
    if (seq < 0 || recv_result(fd, seq, &family) < 0) {
        perror("resolve nl80211");
        return 6;
    }
    seq = send_genl(fd, family, NL80211_CMD_FRAME, NLM_F_ACK, fill_tx, &tx);
    if (seq < 0 || recv_result(fd, seq, NULL) < 0) {
        fprintf(stderr, "NL80211_CMD_FRAME failed: errno=%d (%s)\n",
                errno, strerror(errno));
        return 7;
    }
    printf("NL80211_CMD_FRAME accepted ifindex=%u freq=%u frame_bytes=%zu\n",
           tx.ifindex, tx.freq, tx.frame_len);
    close(fd);
    return 0;
}
