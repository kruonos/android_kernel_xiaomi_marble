#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

static const char *ctrl_dir = "/data/vendor/wifi/hostapd/ctrl";

static int hostapd_request(const char *ifname, const char *cmd,
                           char *reply, size_t reply_len)
{
    char path[256];
    char local_path[256];
    struct sockaddr_un local, dest;
    int s;
    ssize_t res;

    snprintf(path, sizeof(path), "%s/%s", ctrl_dir, ifname);
    snprintf(local_path, sizeof(local_path),
             "%s/hapd_ctrl_%d", ctrl_dir, getpid());

    s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "%s", local_path);
    unlink(local_path);
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }
    chmod(local_path, 0666);
    memset(&dest, 0, sizeof(dest));
    dest.sun_family = AF_UNIX;
    snprintf(dest.sun_path, sizeof(dest.sun_path), "%s", path);

    res = sendto(s, cmd, strlen(cmd), 0, (struct sockaddr *)&dest,
                 sizeof(dest));
    if (res < 0) {
        perror("sendto");
        unlink(local_path);
        close(s);
        return -1;
    }
    {
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    res = recv(s, reply, reply_len - 1, 0);
    unlink(local_path);
    close(s);
    if (res < 0) {
        perror("recv");
        return -1;
    }
    reply[res] = '\0';
    return 0;
}

int main(int argc, char **argv)
{
    char reply[8192];

    if (argc < 2) {
        fprintf(stderr, "usage: %s <iface> <command> [args...]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        if (hostapd_request(argv[1], "GET_CHANNEL", reply, sizeof(reply)) < 0)
            return 3;
        printf("channel: %s", reply);
        return 0;
    }
    if (hostapd_request(argv[1], argv[2], reply, sizeof(reply)) < 0)
        return 3;
    printf("%s\n", reply);
    return 0;
}
