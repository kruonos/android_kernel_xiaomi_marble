/* eth_dump - capture Ethernet-mode frames from a netdev into a pcap file
 * usage: eth_dump <iface> <seconds> <out.pcap>
 */
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

static void put32(FILE *f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

static void put16(FILE *f, uint16_t v)
{
    fwrite(&v, 2, 1, f);
}

int main(int argc, char **argv)
{
    const char *ifname, *outpath;
    int seconds, fd, ifindex;
    unsigned long frames = 0, bytes = 0;
    unsigned char buf[65535];
    struct sockaddr_ll sll = {0};
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    time_t deadline, start;
    FILE *out;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <iface> <seconds> <out.pcap>\n", argv[0]);
        return 2;
    }
    ifname = argv[1];
    seconds = atoi(argv[2]);
    outpath = argv[3];

    ifindex = if_nametoindex(ifname);
    if (!ifindex) {
        perror("if_nametoindex");
        return 2;
    }
    out = fopen(outpath, "wb");
    if (!out) {
        perror("fopen");
        return 3;
    }
    /* pcap global header: little-endian, Ethernet linktype (24 bytes) */
    put32(out, 0xa1b2c3d4);
    put16(out, 2);
    put16(out, 4);
    put32(out, 0);
    put32(out, 0);
    put32(out, 65535);
    put32(out, 1);
    fflush(out);

    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket");
        fclose(out);
        return 4;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt");
        fclose(out);
        close(fd);
        return 5;
    }
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind");
        fclose(out);
        close(fd);
        return 6;
    }

    start = time(NULL);
    deadline = start + seconds;
    while (time(NULL) < deadline) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        struct timeval tv;
        uint32_t ts_sec, ts_usec;

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            perror("recv");
            break;
        }
        gettimeofday(&tv, NULL);
        ts_sec = (uint32_t)tv.tv_sec;
        ts_usec = (uint32_t)tv.tv_usec;
        put32(out, ts_sec);
        put32(out, ts_usec);
        put32(out, (uint32_t)len);
        put32(out, (uint32_t)len);
        fwrite(buf, 1, (size_t)len, out);
        frames++;
        bytes += (unsigned long)len;
    }

    fclose(out);
    close(fd);
    printf("captured %lu frames, %lu bytes in %d s -> %s\n",
           frames, bytes, seconds, outpath);
    return 0;
}
