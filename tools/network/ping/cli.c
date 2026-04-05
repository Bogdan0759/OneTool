#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    const char *host;
    int count;
    int timeout_ms;
    int payload_size;
    double interval_sec;
    int quiet;
} ping_opts_t;

static void ping_help(const char *tool) {
    printf("usage: %s <host> [options]\n", tool);
    printf("options:\n");
    printf("  -c count         number of echo requests\n");
    printf("  -i seconds    interval between packets \n");
    printf("  -W ms>           per packet timeout in ms (default 1000)\n");
    printf("  -s bytes         payload size in bytes\n");
    printf("  -q, --quiet        print only summary\n");
    printf("  -h, --help         show this help\n");
}

static int parse_int(const char *s, int min_v, int max_v, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min_v || v > max_v) {
        return 1;
    }
    *out = (int)v;
    return 0;
}

static int parse_double(const char *s, double min_v, double max_v, double *out) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || v < min_v || v > max_v) {
        return 1;
    }
    *out = v;
    return 0;
}

static int parse_opts(int argc, char *argv[], ping_opts_t *o) {
    memset(o, 0, sizeof(*o));
    o->count = 4;
    o->timeout_ms = 1000;
    o->payload_size = 56;
    o->interval_sec = 1.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            ping_help(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            o->quiet = 1;
            continue;
        }
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc || parse_int(argv[++i], 1, 1000000, &o->count) != 0) {
                fprintf(stderr, "ping: invalid -c value\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-W") == 0) {
            if (i + 1 >= argc || parse_int(argv[++i], 1, 600000, &o->timeout_ms) != 0) {
                fprintf(stderr, "ping: invalid -W value\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc || parse_int(argv[++i], 0, 1400, &o->payload_size) != 0) {
                fprintf(stderr, "ping: invalid -s value\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc || parse_double(argv[++i], 0.01, 3600.0, &o->interval_sec) != 0) {
                fprintf(stderr, "ping: invalid -i value\n");
                return 1;
            }
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "ping: unknown option %s\n", argv[i]);
            return 1;
        }
        if (o->host != NULL) {
            fprintf(stderr, "ping: only one host allowed\n");
            return 1;
        }
        o->host = argv[i];
    }

    if (o->host == NULL) {
        fprintf(stderr, "ping: host required\n");
        return 1;
    }
    return 0;
}

static unsigned short icmp_checksum(const void *data, size_t len) {
    const unsigned short *p = (const unsigned short *)data;
    unsigned int sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const unsigned char *)p;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (unsigned short)~sum;
}

static double ms_since(const struct timeval *start, const struct timeval *end) {
    double sec = (double)(end->tv_sec - start->tv_sec);
    double usec = (double)(end->tv_usec - start->tv_usec);
    return sec * 1000.0 + usec / 1000.0;
}

int main(int argc, char *argv[]) {
    ping_opts_t opt;
    int parse_rc = parse_opts(argc, argv, &opt);
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int sock = -1;
    struct sockaddr_in dst;
    char ipbuf[INET_ADDRSTRLEN];
    int pid_id = getpid() & 0xFFFF;
    int tx = 0;
    int rx = 0;
    double min_rtt = 1e100;
    double max_rtt = 0.0;
    double sum_rtt = 0.0;

    if (parse_rc == 2) {
        return 0;
    }
    if (parse_rc != 0) {
        return 1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_RAW;
    hints.ai_protocol = IPPROTO_ICMP;

    if (getaddrinfo(opt.host, NULL, &hints, &res) != 0 || res == NULL) {
        fprintf(stderr, "ping: cannot resolve host %s\n", opt.host);
        return 1;
    }
    memcpy(&dst, res->ai_addr, sizeof(dst));
    freeaddrinfo(res);

    if (inet_ntop(AF_INET, &dst.sin_addr, ipbuf, sizeof(ipbuf)) == NULL) {
        fprintf(stderr, "ping: inet_ntop failed\n");
        return 1;
    }

    sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        if (errno == EPERM || errno == EACCES) {
            fprintf(stderr, "ping: raw socket requires root or CAP_NET_RAW\n");
        } else {
            fprintf(stderr, "ping: socket failed: %s\n", strerror(errno));
        }
        return 1;
    }

    if (!opt.quiet) {
        printf("PING %s (%s): %d data bytes\n", opt.host, ipbuf, opt.payload_size);
    }

    for (int seq = 1; seq <= opt.count; seq++) {
        unsigned char packet[1500];
        unsigned char recvbuf[2048];
        struct icmphdr *icmp = (struct icmphdr *)packet;
        struct timeval send_tv;
        struct timeval recv_tv;
        struct pollfd pfd;
        int pkt_len = (int)sizeof(struct icmphdr) + (int)sizeof(struct timeval) + opt.payload_size;
        int poll_rc;

        if (pkt_len > (int)sizeof(packet)) {
            fprintf(stderr, "ping: payload too large\n");
            close(sock);
            return 1;
        }

        memset(packet, 0, (size_t)pkt_len);
        icmp->type = ICMP_ECHO;
        icmp->code = 0;
        icmp->un.echo.id = htons((unsigned short)pid_id);
        icmp->un.echo.sequence = htons((unsigned short)seq);

        gettimeofday(&send_tv, NULL);
        memcpy(packet + sizeof(struct icmphdr), &send_tv, sizeof(send_tv));
        for (int i = 0; i < opt.payload_size; i++) {
            packet[sizeof(struct icmphdr) + sizeof(struct timeval) + i] = (unsigned char)(i & 0xFF);
        }
        icmp->checksum = icmp_checksum(packet, (size_t)pkt_len);

        tx++;
        if (sendto(sock, packet, (size_t)pkt_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            fprintf(stderr, "ping: sendto failed: %s\n", strerror(errno));
            continue;
        }

        pfd.fd = sock;
        pfd.events = POLLIN;
        poll_rc = poll(&pfd, 1, opt.timeout_ms);
        if (poll_rc == 0) {
            if (!opt.quiet) {
                printf("request timeout icmp_seq %d\n", seq);
            }
        } else if (poll_rc < 0) {
            if (errno != EINTR) {
                fprintf(stderr, "ping: poll %s failed\n", strerror(errno));
            }
        } else {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(sock, recvbuf, sizeof(recvbuf), 0, (struct sockaddr *)&from, &from_len);
            if (n > (ssize_t)(sizeof(struct iphdr) + sizeof(struct icmphdr))) {
                struct iphdr *iph = (struct iphdr *)recvbuf;
                int ihl = iph->ihl * 4;
                struct icmphdr *ri = (struct icmphdr *)(recvbuf + ihl);
                if (ri->type == ICMP_ECHOREPLY && ntohs(ri->un.echo.id) == (unsigned short)pid_id) {
                    struct timeval sent_in_pkt;
                    double rtt;
                    gettimeofday(&recv_tv, NULL);
                    memcpy(&sent_in_pkt, recvbuf + ihl + sizeof(struct icmphdr), sizeof(sent_in_pkt));
                    rtt = ms_since(&sent_in_pkt, &recv_tv);
                    if (rtt < min_rtt) min_rtt = rtt;
                    if (rtt > max_rtt) max_rtt = rtt;
                    sum_rtt += rtt;
                    rx++;
                    if (!opt.quiet) {
                        char from_ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));
                        printf("%zd bytes from %s: icmp_seq=%d ttl=%d time=%.3f ms\n",
                               n - ihl, from_ip, ntohs(ri->un.echo.sequence), iph->ttl, rtt);
                    }
                } else if (!opt.quiet) {
                    printf("unexpected ICMP packet for seq %d\n", seq);
                }
            } else if (!opt.quiet) {
                printf("short packet received for seq %d\n", seq);
            }
        }

        if (seq != opt.count) {
            useconds_t us = (useconds_t)(opt.interval_sec * 1000000.0);
            usleep(us);
        }
    }

    printf("\n%s\n", opt.host);
    if (tx > 0) {
        double loss = 100.0 * (double)(tx - rx) / (double)tx;
        printf("%d packets transmitted %d received %.1f%% packet loss\n", tx, rx, loss);
    } else {
        printf("0 packets transmitted\n");
    }
    if (rx > 0) {
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", min_rtt, sum_rtt / (double)rx, max_rtt);
    }

    close(sock);
    return rx > 0 ? 0 : 1;
}
