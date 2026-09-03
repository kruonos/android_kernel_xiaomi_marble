#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_SA 256

struct sa_entry {
    unsigned char mac[6];
    unsigned long count;
};

static const char *mac_str(const unsigned char *m)
{
    static char out[18];
    snprintf(out, sizeof(out), "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return out;
}

static void put32(FILE *f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

static void put16(FILE *f, uint16_t v)
{
    fwrite(&v, 2, 1, f);
}

/* pcap linktype 127 = LINKTYPE_IEEE802_11_RADIOTAP */
static FILE *pcap_open(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen");
        return NULL;
    }
    put32(f, 0xa1b2c3d4);
    put16(f, 2);
    put16(f, 4);
    put32(f, 0);
    put32(f, 0);
    put32(f, 65535);
    put32(f, 127);
    fflush(f);
    return f;
}

int main(int argc, char **argv)
{
    const char *ifname = argc > 1 ? argv[1] : "mon0";
    int seconds = argc > 2 ? atoi(argv[2]) : 15;
    const char *outpath = argc > 3 ? argv[3] : NULL;
    int fd, ifindex, count = 0, radiotap = 0, saved = 0;
    unsigned char buf[8192];
    struct sockaddr_ll sll = {0};
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    time_t deadline;
    FILE *out = NULL;

    /* per-type counters */
    unsigned long beacon = 0, probe_req = 0, probe_resp = 0, auth = 0;
    unsigned long deauth = 0, mgmt_other = 0, ctrl = 0;
    unsigned long data_qos = 0, data_null = 0, data_other = 0;
    unsigned long bytes_total = 0;
    struct sa_entry sa_table[MAX_SA];
    int sa_count = 0;
    int i;

    memset(sa_table, 0, sizeof(sa_table));

    if (outpath) {
        out = pcap_open(outpath);
        if (!out)
            return 7;
    }

    ifindex = if_nametoindex(ifname);
    if (!ifindex) {
        perror("if_nametoindex");
        return 2;
    }
    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket");
        return 3;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt");
        close(fd);
        return 4;
    }
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind");
        close(fd);
        return 5;
    }

    deadline = time(NULL) + seconds;
    while (time(NULL) < deadline) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        unsigned int hdrlen;
        const unsigned char *f;
        unsigned char fc0, fc1, type, sub;
        int i, found;

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            perror("recv");
            close(fd);
            return 6;
        }
        count++;
        bytes_total += (unsigned long)len;
        if (!(len >= 4 && buf[0] == 0 && buf[1] == 0))
            continue;
        hdrlen = (unsigned int)buf[2] | ((unsigned int)buf[3] << 8);
        if (!(hdrlen >= 8 && hdrlen <= (unsigned int)len))
            continue;
        radiotap++;

        if (out) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            put32(out, (uint32_t)tv.tv_sec);
            put32(out, (uint32_t)tv.tv_usec);
            put32(out, (uint32_t)len);
            put32(out, (uint32_t)len);
            fwrite(buf, 1, (size_t)len, out);
            saved++;
        }

        f = buf + hdrlen;
        if ((size_t)len < hdrlen + 10)
            continue;
        fc0 = f[0];
        fc1 = f[1];
        type = (fc0 >> 2) & 0x3;
        sub = (fc0 >> 4) & 0xf;

        if (type == 0) {
            if (fc0 == 0x80) beacon++;
            else if (fc0 == 0x40) probe_req++;
            else if (fc0 == 0x50) probe_resp++;
            else if (fc0 == 0xb0) auth++;
            else if (fc0 == 0xc0) deauth++;
            else mgmt_other++;
        } else if (type == 1) {
            ctrl++;
        } else if (type == 2) {
            if (fc0 == 0x88 || fc0 == 0x89 || fc0 == 0x8a || fc0 == 0x8b)
                data_qos++;
            else if (fc0 == 0xc8 || fc0 == 0xc9 || fc0 == 0xca || fc0 == 0xcb)
                data_null++;
            else
                data_other++;
        }

        /* unique transmitters (address 2) */
        found = 0;
        for (i = 0; i < sa_count; i++) {
            if (!memcmp(sa_table[i].mac, f + 10, 6)) {
                sa_table[i].count++;
                found = 1;
                break;
            }
        }
        if (!found && sa_count < MAX_SA) {
            memcpy(sa_table[sa_count].mac, f + 10, 6);
            sa_table[sa_count].count = 1;
            sa_count++;
        }

        if (count <= 3)
            printf("frame=%d bytes=%zd radiotap_len=%u fc=%02x%02x\n",
                   count, len, hdrlen, fc0, fc1);
    }

    if (out) {
        fflush(out);
        fclose(out);
    }
    printf("\ncapture: %d s  frames=%d  radiotap=%d  bytes=%lu\n",
           seconds, count, radiotap, bytes_total);
    if (outpath)
        printf("  saved %d frames -> %s\n", saved, outpath);
    printf("  beacons        : %lu\n", beacon);
    printf("  probe requests : %lu\n", probe_req);
    printf("  probe responses: %lu\n", probe_resp);
    printf("  auth           : %lu\n", auth);
    printf("  deauth         : %lu\n", deauth);
    printf("  mgmt other     : %lu\n", mgmt_other);
    printf("  control (ack/cts/rts): %lu\n", ctrl);
    printf("  data qos       : %lu\n", data_qos);
    printf("  data null (power save): %lu\n", data_null);
    printf("  data other     : %lu\n", data_other);
    printf("  unique transmitters : %d\n", sa_count);
    for (i = 0; i < sa_count && i < 20; i++) {
        printf("    %s  x%lu\n", mac_str(sa_table[i].mac),
               sa_table[i].count);
    }
    if (sa_count > 20)
        printf("    ... %d more\n", sa_count - 20);
    close(fd);
    return radiotap ? 0 : 1;
}
