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

#define QCA_OUI 0x001374
#define QCA_NL80211_VENDOR_SUBCMD_DO_ACS 54
#define NUM_CHANNELS 102

enum {
    QCA_WLAN_VENDOR_ATTR_ACS_HW_MODE = 3,
    QCA_WLAN_VENDOR_ATTR_ACS_VHT_ENABLED = 6,
    QCA_WLAN_VENDOR_ATTR_ACS_CHWIDTH = 7,
    QCA_WLAN_VENDOR_ATTR_ACS_FREQ_LIST = 11,
    QCA_WLAN_VENDOR_ATTR_ACS_EHT_ENABLED = 19,
};

static int seqno = 400;

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

static void *put_nla(void *pos, int type, const void *data, int data_len)
{
    struct nlattr *a = pos;
    a->nla_type = type;
    a->nla_len = NLA_HDRLEN + data_len;
    if (data_len)
        memcpy(NLA_DATA(a), data, data_len);
    return (char *)pos + NLA_ALIGN(a->nla_len);
}

static int send_genl(int fd, uint16_t family, uint8_t cmd, int flags,
                     void (*fill)(struct nlmsghdr *, int, void *), void *arg)
{
    unsigned char buf[8192] = {0};
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
    unsigned char buf[16384];
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
                int errlen = (int)n->nlmsg_len - NLMSG_LENGTH(sizeof(*e));
                if (errlen > 0) {
                    int rem = errlen;
                    for (struct nlattr *ea = (struct nlattr *)((char *)e +
                             sizeof(*e)); NLA_OK(ea, rem);
                         ea = NLA_NEXT(ea, rem)) {
                        if (ea->nla_type == NLMSGERR_ATTR_MSG)
                            fprintf(stderr, "  extack: %s\n",
                                    (char *)NLA_DATA(ea));
                        if (ea->nla_type == NLMSGERR_ATTR_OFFS)
                            fprintf(stderr, "  bad attr offset: %u\n",
                                    *(uint32_t *)NLA_DATA(ea));
                    }
                }
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
                    if (a->nla_type == CTRL_ATTR_FAMILY_ID &&
                        NLA_PAYLOAD(a) >= 2) {
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

struct acs_args {
    uint32_t ifindex;
    uint32_t *freqs;
    int num_freqs;
};

static void fill_acs(struct nlmsghdr *n, int maxlen, void *arg)
{
    struct acs_args *a = arg;
    unsigned char inner[4096];
    void *pos = inner;
    uint8_t hw_mode = 2;   /* 802.11a */
    uint16_t chwidth = 0;  /* 20 MHz */
    static uint32_t freq_list[NUM_CHANNELS];
    int i;

    memset(freq_list, 0, sizeof(freq_list));
    for (i = 0; i < a->num_freqs && i < NUM_CHANNELS; i++)
        freq_list[i] = a->freqs[i];

    pos = put_nla(pos, QCA_WLAN_VENDOR_ATTR_ACS_HW_MODE, &hw_mode, 1);
    if (a->num_freqs > 0)
        pos = put_nla(pos, QCA_WLAN_VENDOR_ATTR_ACS_FREQ_LIST, freq_list,
                      sizeof(freq_list));
    pos = put_nla(pos, QCA_WLAN_VENDOR_ATTR_ACS_CHWIDTH, &chwidth, 2);
    pos = put_nla(pos, QCA_WLAN_VENDOR_ATTR_ACS_VHT_ENABLED, NULL, 0);
    pos = put_nla(pos, QCA_WLAN_VENDOR_ATTR_ACS_EHT_ENABLED, NULL, 0);

    addattr(n, maxlen, NL80211_ATTR_VENDOR_ID, &(uint32_t){QCA_OUI}, 4);
    addattr(n, maxlen, NL80211_ATTR_VENDOR_SUBCMD,
            &(uint32_t){QCA_NL80211_VENDOR_SUBCMD_DO_ACS}, 4);
    addattr(n, maxlen, NL80211_ATTR_IFINDEX, &a->ifindex, 4);
    addattr(n, maxlen, NL80211_ATTR_VENDOR_DATA, inner,
            (int)((char *)pos - (char *)inner));
}

int main(int argc, char **argv)
{
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    struct acs_args acs = {0};
    int fd, family = -1, seq;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <sap-iface> <freq-mhz> [freq2 ...] | <sap-iface> probe\n",
                argv[0]);
        return 2;
    }
    acs.ifindex = if_nametoindex(argv[1]);
    if (!acs.ifindex) {
        fprintf(stderr, "invalid interface\n");
        return 3;
    }
    if (!strcmp(argv[2], "probe")) {
        acs.num_freqs = 0;
    } else {
        acs.num_freqs = argc - 2;
        acs.freqs = calloc((size_t)acs.num_freqs, sizeof(uint32_t));
        for (int i = 0; i < acs.num_freqs; i++)
            acs.freqs[i] = strtoul(argv[i + 2], NULL, 10);
    }

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
    seq = send_genl(fd, family, NL80211_CMD_VENDOR, NLM_F_ACK, fill_acs, &acs);
    if (seq < 0 || recv_result(fd, seq, NULL) < 0) {
        fprintf(stderr, "DO_ACS failed: errno=%d (%s)\n", errno,
                strerror(errno));
        return 7;
    }
    printf("DO_ACS accepted ifindex=%u freqs=%d\n", acs.ifindex,
           acs.num_freqs);
    close(fd);
    return 0;
}
