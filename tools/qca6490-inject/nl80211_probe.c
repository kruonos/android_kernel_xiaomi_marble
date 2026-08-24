// Minimal nl80211 probe for Android/aarch64 static use.
// Read-only by default. Optional add/del monitor interface commands are transient tests.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef NLA_F_NESTED
#define NLA_F_NESTED (1 << 15)
#endif
#ifndef NLA_F_NET_BYTEORDER
#define NLA_F_NET_BYTEORDER (1 << 14)
#endif
#ifndef NLA_TYPE_MASK
#define NLA_TYPE_MASK ~(NLA_F_NESTED | NLA_F_NET_BYTEORDER)
#endif
#ifndef NLA_ALIGNTO
#define NLA_ALIGNTO 4
#endif
#ifndef NLA_ALIGN
#define NLA_ALIGN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#endif
#ifndef NLA_HDRLEN
#define NLA_HDRLEN ((int) NLA_ALIGN(sizeof(struct nlattr)))
#endif
#define NLA_DATA(nla) ((void *)((char *)(nla) + NLA_HDRLEN))
#define NLA_PAYLOAD(nla) ((int)((nla)->nla_len - NLA_HDRLEN))
#define NLA_OK(nla, len) ((len) >= (int)sizeof(struct nlattr) && \
                         (nla)->nla_len >= sizeof(struct nlattr) && \
                         (nla)->nla_len <= (len))
#define NLA_NEXT(nla, attrlen) ((attrlen) -= NLA_ALIGN((nla)->nla_len), \
                               (struct nlattr *)(((char *)(nla)) + NLA_ALIGN((nla)->nla_len)))
#define NLMSG_TAIL(nmsg) ((struct nlattr *)(((char *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

#define BUF_SIZE 65536

static int seq_no = 100;

static const char *iftype_name(int t)
{
    switch (t) {
    case NL80211_IFTYPE_UNSPECIFIED: return "unspecified";
    case NL80211_IFTYPE_ADHOC: return "IBSS/ad-hoc";
    case NL80211_IFTYPE_STATION: return "station";
    case NL80211_IFTYPE_AP: return "AP";
    case NL80211_IFTYPE_AP_VLAN: return "AP/VLAN";
    case NL80211_IFTYPE_WDS: return "WDS";
    case NL80211_IFTYPE_MONITOR: return "monitor";
    case NL80211_IFTYPE_MESH_POINT: return "mesh";
    case NL80211_IFTYPE_P2P_CLIENT: return "P2P-client";
    case NL80211_IFTYPE_P2P_GO: return "P2P-GO";
    case NL80211_IFTYPE_P2P_DEVICE: return "P2P-device";
    case NL80211_IFTYPE_OCB: return "OCB";
    case NL80211_IFTYPE_NAN: return "NAN";
    default: return "unknown";
    }
}

static int attr_type(const struct nlattr *a)
{
    return a->nla_type & NLA_TYPE_MASK;
}

static void addattr(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen)
{
    int len = NLA_HDRLEN + alen;
    struct nlattr *na;
    if ((int)NLMSG_ALIGN(n->nlmsg_len) + NLA_ALIGN(len) > maxlen) {
        fprintf(stderr, "addattr overflow\n");
        exit(2);
    }
    na = NLMSG_TAIL(n);
    na->nla_type = type;
    na->nla_len = len;
    if (alen && data)
        memcpy(NLA_DATA(na), data, alen);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + NLA_ALIGN(len);
}

static void addattr_u32(struct nlmsghdr *n, int maxlen, int type, uint32_t val)
{
    addattr(n, maxlen, type, &val, sizeof(val));
}

static void addattr_str(struct nlmsghdr *n, int maxlen, int type, const char *s)
{
    addattr(n, maxlen, type, s, strlen(s) + 1);
}

static int nl_send(int fd, uint16_t nlmsg_type, uint8_t genl_cmd, uint8_t genl_version,
                   int flags, void (*fill)(struct nlmsghdr *n, int maxlen, void *arg), void *arg)
{
    char buf[4096];
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct genlmsghdr *ghdr;
    struct sockaddr_nl nladdr = {0};

    memset(buf, 0, sizeof(buf));
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ghdr));
    nlh->nlmsg_type = nlmsg_type;
    nlh->nlmsg_flags = NLM_F_REQUEST | flags;
    nlh->nlmsg_seq = ++seq_no;
    nlh->nlmsg_pid = (uint32_t)getpid();
    ghdr = (struct genlmsghdr *)NLMSG_DATA(nlh);
    ghdr->cmd = genl_cmd;
    ghdr->version = genl_version;

    if (fill)
        fill(nlh, sizeof(buf), arg);

    nladdr.nl_family = AF_NETLINK;
    if (sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr)) < 0) {
        perror("sendto");
        return -1;
    }
    return nlh->nlmsg_seq;
}

static int recv_ack_or_error(int fd, int seq)
{
    char buf[BUF_SIZE];
    while (1) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            return -1;
        }
        for (struct nlmsghdr *nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len)) {
            if ((int)nlh->nlmsg_seq != seq)
                continue;
            if (nlh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
                if (err->error == 0)
                    return 0;
                errno = -err->error;
                return -1;
            }
            if (nlh->nlmsg_type == NLMSG_DONE)
                return 0;
        }
    }
}

typedef int (*msg_cb)(struct nlmsghdr *nlh, void *arg);

static int recv_dump(int fd, int seq, msg_cb cb, void *arg)
{
    char buf[BUF_SIZE];
    int done = 0;
    while (!done) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            return -1;
        }
        for (struct nlmsghdr *nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len)) {
            if ((int)nlh->nlmsg_seq != seq)
                continue;
            if (nlh->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }
            if (nlh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
                if (err->error) {
                    errno = -err->error;
                    perror("netlink error");
                    return -1;
                }
                done = 1;
                break;
            }
            if (cb && cb(nlh, arg))
                return 1;
        }
    }
    return 0;
}

struct family_state { int family_id; };

static void fill_family_name(struct nlmsghdr *n, int maxlen, void *arg)
{
    (void)arg;
    addattr_str(n, maxlen, CTRL_ATTR_FAMILY_NAME, "nl80211");
}

static int parse_family_msg(struct nlmsghdr *nlh, void *arg)
{
    struct family_state *st = arg;
    struct genlmsghdr *ghdr = NLMSG_DATA(nlh);
    int len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ghdr));
    for (struct nlattr *a = (struct nlattr *)((char *)ghdr + GENL_HDRLEN); NLA_OK(a, len); a = NLA_NEXT(a, len)) {
        if (attr_type(a) == CTRL_ATTR_FAMILY_ID && NLA_PAYLOAD(a) >= 2) {
            st->family_id = *(uint16_t *)NLA_DATA(a);
            return 1;
        }
    }
    return 0;
}

static int resolve_nl80211(int fd)
{
    struct family_state st = { .family_id = -1 };
    int seq = nl_send(fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1, 0, fill_family_name, NULL);
    if (seq < 0)
        return -1;
    recv_dump(fd, seq, parse_family_msg, &st);
    return st.family_id;
}

struct wiphy_info {
    uint32_t first_wiphy;
    int have_first;
    int saw_monitor;
};

static int parse_wiphy_msg(struct nlmsghdr *nlh, void *arg)
{
    struct wiphy_info *info = arg;
    struct genlmsghdr *ghdr = NLMSG_DATA(nlh);
    int len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ghdr));
    int wiphy = -1;
    const char *name = NULL;
    struct nlattr *iftypes = NULL;

    for (struct nlattr *a = (struct nlattr *)((char *)ghdr + GENL_HDRLEN); NLA_OK(a, len); a = NLA_NEXT(a, len)) {
        int t = attr_type(a);
        if (t == NL80211_ATTR_WIPHY && NLA_PAYLOAD(a) >= 4)
            wiphy = *(uint32_t *)NLA_DATA(a);
        else if (t == NL80211_ATTR_WIPHY_NAME)
            name = (const char *)NLA_DATA(a);
        else if (t == NL80211_ATTR_SUPPORTED_IFTYPES)
            iftypes = a;
    }

    if (wiphy >= 0 && !info->have_first) {
        info->first_wiphy = (uint32_t)wiphy;
        info->have_first = 1;
    }

    if (wiphy >= 0 || name || iftypes) {
        printf("wiphy=%d name=%s\n", wiphy, name ? name : "?");
    }

    if (iftypes) {
        int rem = NLA_PAYLOAD(iftypes);
        printf("  supported_iftypes:");
        for (struct nlattr *b = (struct nlattr *)NLA_DATA(iftypes); NLA_OK(b, rem); b = NLA_NEXT(b, rem)) {
            int it = attr_type(b);
            printf(" %s(%d)", iftype_name(it), it);
            if (it == NL80211_IFTYPE_MONITOR)
                info->saw_monitor = 1;
        }
        printf("\n");
    }
    return 0;
}

static int parse_iface_msg(struct nlmsghdr *nlh, void *arg)
{
    (void)arg;
    struct genlmsghdr *ghdr = NLMSG_DATA(nlh);
    int len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ghdr));
    int ifidx = -1, iftype = -1, wiphy = -1;
    const char *ifname = NULL;

    for (struct nlattr *a = (struct nlattr *)((char *)ghdr + GENL_HDRLEN); NLA_OK(a, len); a = NLA_NEXT(a, len)) {
        int t = attr_type(a);
        if (t == NL80211_ATTR_IFINDEX && NLA_PAYLOAD(a) >= 4)
            ifidx = *(uint32_t *)NLA_DATA(a);
        else if (t == NL80211_ATTR_IFNAME)
            ifname = (const char *)NLA_DATA(a);
        else if (t == NL80211_ATTR_IFTYPE && NLA_PAYLOAD(a) >= 4)
            iftype = *(uint32_t *)NLA_DATA(a);
        else if (t == NL80211_ATTR_WIPHY && NLA_PAYLOAD(a) >= 4)
            wiphy = *(uint32_t *)NLA_DATA(a);
    }
    if (ifidx >= 0 || ifname || iftype >= 0)
        printf("ifindex=%d ifname=%s iftype=%s(%d) wiphy=%d\n", ifidx, ifname ? ifname : "?", iftype_name(iftype), iftype, wiphy);
    return 0;
}

struct find_iface_state { const char *name; int ifindex; };

static int find_iface_msg(struct nlmsghdr *nlh, void *arg)
{
    struct find_iface_state *st = arg;
    struct genlmsghdr *ghdr = NLMSG_DATA(nlh);
    int len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ghdr));
    int ifidx = -1;
    const char *ifname = NULL;
    for (struct nlattr *a = (struct nlattr *)((char *)ghdr + GENL_HDRLEN); NLA_OK(a, len); a = NLA_NEXT(a, len)) {
        int t = attr_type(a);
        if (t == NL80211_ATTR_IFINDEX && NLA_PAYLOAD(a) >= 4)
            ifidx = *(uint32_t *)NLA_DATA(a);
        else if (t == NL80211_ATTR_IFNAME)
            ifname = (const char *)NLA_DATA(a);
    }
    if (ifname && strcmp(ifname, st->name) == 0) {
        st->ifindex = ifidx;
        return 1;
    }
    return 0;
}

static int get_first_wiphy(int fd, int family_id, struct wiphy_info *info)
{
    memset(info, 0, sizeof(*info));
    int seq = nl_send(fd, family_id, NL80211_CMD_GET_WIPHY, 0, NLM_F_DUMP, NULL, NULL);
    if (seq < 0)
        return -1;
    return recv_dump(fd, seq, parse_wiphy_msg, info);
}

static int list_ifaces(int fd, int family_id)
{
    int seq = nl_send(fd, family_id, NL80211_CMD_GET_INTERFACE, 0, NLM_F_DUMP, NULL, NULL);
    if (seq < 0)
        return -1;
    return recv_dump(fd, seq, parse_iface_msg, NULL);
}

static int find_iface(int fd, int family_id, const char *name)
{
    struct find_iface_state st = { .name = name, .ifindex = -1 };
    int seq = nl_send(fd, family_id, NL80211_CMD_GET_INTERFACE, 0, NLM_F_DUMP, NULL, NULL);
    if (seq < 0)
        return -1;
    recv_dump(fd, seq, find_iface_msg, &st);
    return st.ifindex;
}

struct addmon_arg { uint32_t wiphy; const char *name; };
static void fill_new_mon(struct nlmsghdr *n, int maxlen, void *arg)
{
    struct addmon_arg *a = arg;
    addattr_u32(n, maxlen, NL80211_ATTR_WIPHY, a->wiphy);
    addattr_str(n, maxlen, NL80211_ATTR_IFNAME, a->name);
    addattr_u32(n, maxlen, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_MONITOR);
}

struct del_arg { uint32_t ifindex; };
static void fill_del_iface(struct nlmsghdr *n, int maxlen, void *arg)
{
    struct del_arg *a = arg;
    addattr_u32(n, maxlen, NL80211_ATTR_IFINDEX, a->ifindex);
}

static int add_monitor(int fd, int family_id, uint32_t wiphy, const char *name)
{
    struct addmon_arg arg = { .wiphy = wiphy, .name = name };
    int seq = nl_send(fd, family_id, NL80211_CMD_NEW_INTERFACE, 0, NLM_F_ACK, fill_new_mon, &arg);
    if (seq < 0)
        return -1;
    int ret = recv_ack_or_error(fd, seq);
    if (ret < 0) {
        fprintf(stderr, "NEW_INTERFACE monitor failed: errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    printf("NEW_INTERFACE monitor OK: %s\n", name);
    return 0;
}

static int del_iface_by_name(int fd, int family_id, const char *name)
{
    int ifidx = find_iface(fd, family_id, name);
    if (ifidx < 0) {
        printf("DEL_INTERFACE skipped: %s not found\n", name);
        return 0;
    }
    struct del_arg arg = { .ifindex = (uint32_t)ifidx };
    int seq = nl_send(fd, family_id, NL80211_CMD_DEL_INTERFACE, 0, NLM_F_ACK, fill_del_iface, &arg);
    if (seq < 0)
        return -1;
    int ret = recv_ack_or_error(fd, seq);
    if (ret < 0) {
        fprintf(stderr, "DEL_INTERFACE %s/%d failed: errno=%d (%s)\n", name, ifidx, errno, strerror(errno));
        return -1;
    }
    printf("DEL_INTERFACE OK: %s ifindex=%d\n", name, ifidx);
    return 0;
}

static int open_nl(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) {
        perror("socket NETLINK_GENERIC");
        return -1;
    }
    struct sockaddr_nl local = {0};
    local.nl_family = AF_NETLINK;
    local.nl_pid = (uint32_t)getpid();
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind NETLINK_GENERIC");
        close(fd);
        return -1;
    }
    return fd;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [probe|addmon [name]|del [name]|ifaces]\n", argv0);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "probe";
    const char *name = argc > 2 ? argv[2] : "mon0";
    int fd = open_nl();
    if (fd < 0)
        return 2;
    int family_id = resolve_nl80211(fd);
    if (family_id < 0) {
        fprintf(stderr, "nl80211 family not found\n");
        close(fd);
        return 3;
    }
    printf("nl80211_family_id=%d\n", family_id);

    if (strcmp(cmd, "ifaces") == 0) {
        int r = list_ifaces(fd, family_id);
        close(fd);
        return r ? 1 : 0;
    }
    if (strcmp(cmd, "del") == 0) {
        int r = del_iface_by_name(fd, family_id, name);
        close(fd);
        return r ? 1 : 0;
    }

    struct wiphy_info info;
    if (get_first_wiphy(fd, family_id, &info) < 0) {
        close(fd);
        return 4;
    }
    printf("monitor_iftype_advertised=%s\n", info.saw_monitor ? "yes" : "no");
    printf("interfaces:\n");
    list_ifaces(fd, family_id);

    if (strcmp(cmd, "addmon") == 0) {
        if (!info.have_first) {
            fprintf(stderr, "no wiphy found\n");
            close(fd);
            return 5;
        }
        int r = add_monitor(fd, family_id, info.first_wiphy, name);
        printf("interfaces_after_new:\n");
        list_ifaces(fd, family_id);
        close(fd);
        return r ? 1 : 0;
    }

    if (strcmp(cmd, "probe") != 0) {
        usage(argv[0]);
        close(fd);
        return 64;
    }
    close(fd);
    return 0;
}
