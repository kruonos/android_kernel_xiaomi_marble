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
#include <unistd.h>

#define MAX_FRAME 2048

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
    const char *ifname = argv[1];
    const char *src = argv[2];
    int count = argc > 3 ? atoi(argv[3]) : 1;
    int interval_ms = argc > 4 ? atoi(argv[4]) : 250;
    unsigned char frame[MAX_FRAME];
    unsigned char pkt[MAX_FRAME + 8];
    struct sockaddr_ll sll = {0};
    FILE *fp;
    int fd, ifindex, frame_len;
    size_t got;

    if (argc < 3 || argc > 5) {
        fprintf(stderr,
                "usage: %s <iface> <frame-file-or-hex> [count] [interval_ms]\n",
                argv[0]);
        return 2;
    }
    ifindex = if_nametoindex(ifname);
    if (!ifindex) {
        perror("if_nametoindex");
        return 3;
    }
    fp = fopen(src, "rb");
    if (fp) {
        got = fread(frame, 1, sizeof(frame), fp);
        fclose(fp);
        if (got < 10 || got > sizeof(frame)) {
            fprintf(stderr, "frame file must be 10..%u bytes\n", MAX_FRAME);
            return 4;
        }
        frame_len = (int)got;
    } else {
        frame_len = parse_hex(src, frame, sizeof(frame));
        if (frame_len < 0) {
            fprintf(stderr, "cannot read frame file and input is not hex\n");
            return 4;
        }
    }

    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket");
        return 5;
    }
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind");
        close(fd);
        return 6;
    }

    memset(pkt, 0, 8);
    pkt[2] = 0x08;
    memcpy(pkt + 8, frame, (size_t)frame_len);

    for (int i = 0; i < count; i++) {
        ssize_t sent = sendto(fd, pkt, (size_t)frame_len + 8, 0,
                              (struct sockaddr *)&sll, sizeof(sll));
        if (sent < 0) {
            fprintf(stderr, "send_%d failed: errno=%d (%s)\n", i + 1,
                    errno, strerror(errno));
            close(fd);
            return 7;
        }
        printf("send_%d frame_bytes=%d\n", i + 1, frame_len);
        if (i + 1 < count)
            usleep((useconds_t)interval_ms * 1000);
    }
    close(fd);
    return 0;
}
