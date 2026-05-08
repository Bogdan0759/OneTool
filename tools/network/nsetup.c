#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <netpacket/packet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC_COOKIE 0x63825363U
#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY 2
#define DHCP_HTYPE_ETHERNET 1
#define DHCP_HLEN_ETHERNET 6
#define DHCP_FLAG_BROADCAST 0x8000

#define DHCPDISCOVER 1
#define DHCPOFFER 2
#define DHCPREQUEST 3
#define DHCPACK 5
#define DHCPNAK 6

#define DHCP_OPTION_SUBNET_MASK 1
#define DHCP_OPTION_ROUTER 3
#define DHCP_OPTION_DNS 6
#define DHCP_OPTION_HOSTNAME 12
#define DHCP_OPTION_DOMAIN_NAME 15
#define DHCP_OPTION_REQUESTED_IP 50
#define DHCP_OPTION_LEASE_TIME 51
#define DHCP_OPTION_MESSAGE_TYPE 53
#define DHCP_OPTION_SERVER_ID 54
#define DHCP_OPTION_PARAM_REQ_LIST 55
#define DHCP_OPTION_CLIENT_ID 61
#define DHCP_OPTION_END 255

#define DHCP_TIMEOUT_MS 8000
#define DHCP_PACKET_MAX 1500
#define MAX_DNS_SERVERS 4
#define MAX_ROUTERS 4

typedef struct {
    char name[IFNAMSIZ];
    unsigned int flags;
    unsigned char mac[6];
    int has_mac;
    char operstate[32];
} iface_info_t;

typedef struct {
    uint32_t yiaddr;
    uint32_t subnet_mask;
    uint32_t server_id;
    uint32_t routers[MAX_ROUTERS];
    int router_count;
    uint32_t dns[MAX_DNS_SERVERS];
    int dns_count;
    uint32_t lease_time;
    char domain[128];
} dhcp_lease_t;

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    unsigned char chaddr[16];
    unsigned char sname[64];
    unsigned char file[128];
    uint32_t cookie;
    unsigned char options[312];
} dhcp_packet_t;

static void ipv4_to_str(uint32_t addr, char *buf, size_t buf_size) {
    struct in_addr in;

    in.s_addr = addr;
    if (inet_ntop(AF_INET, &in, buf, buf_size) == NULL) {
        snprintf(buf, buf_size, "0.0.0.0");
    }
}

static long long now_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

static int read_operstate(const char *ifname, char *buf, size_t buf_size) {
    char path[256];
    FILE *fp;

    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }
    if (fgets(buf, (int)buf_size, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}
static int find_iface(iface_info_t *list, size_t count, const char *name) {
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int collect_interfaces(iface_info_t **out_list, size_t *out_count) {
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;
    iface_info_t *list = NULL;
    size_t count = 0;

    if (getifaddrs(&ifaddr) != 0) {
        fprintf(stderr, "nsetup: getifaddrs failed: %s\n", strerror(errno));
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        iface_info_t *entry;
        int idx;

        if (ifa->ifa_name == NULL) {
            continue;
        }

        idx = find_iface(list, count, ifa->ifa_name);
        if (idx < 0) {
            iface_info_t *next = realloc(list, (count + 1) * sizeof(*next));
            if (next == NULL) {
                fprintf(stderr, "nsetup: out of memory\n");
                free(list);
                freeifaddrs(ifaddr);
                return -1;
            }
            list = next;
            entry = &list[count];
            memset(entry, 0, sizeof(*entry));
            snprintf(entry->name, sizeof(entry->name), "%s", ifa->ifa_name);
            if (read_operstate(entry->name, entry->operstate, sizeof(entry->operstate)) != 0) {
                snprintf(entry->operstate, sizeof(entry->operstate), "unknown");
            }
            count++;
        } else {
            entry = &list[idx];
        }

        entry->flags |= ifa->ifa_flags;

        if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_PACKET) {
            struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;

            if (sll->sll_halen == 6) {
                memcpy(entry->mac, sll->sll_addr, 6);
                entry->has_mac = 1;
            }
        }
    }

    freeifaddrs(ifaddr);
    *out_list = list;
    *out_count = count;
    return 0;
}

static void print_interfaces(const iface_info_t *list, size_t count) {
    size_t i;

    printf("available interfaces:\n");
    for (i = 0; i < count; i++) {
        char mac_buf[32] = "-";
        const char *state = (list[i].flags & IFF_UP) ? "up" : "down";

        if (list[i].has_mac) {
            snprintf(
                mac_buf,
                sizeof(mac_buf),
                "%02x:%02x:%02x:%02x:%02x:%02x",
                list[i].mac[0],
                list[i].mac[1],
                list[i].mac[2],
                list[i].mac[3],
                list[i].mac[4],
                list[i].mac[5]
            );
        }

        printf(
            "  %-12s state=%-4s link=%-10s mac=%s%s\n",
            list[i].name,
            state,
            list[i].operstate[0] ? list[i].operstate : "unknown",
            mac_buf,
            (list[i].flags & IFF_LOOPBACK) ? " loopback" : ""
        );
    }
}

static int get_iface_mac(const char *ifname, unsigned char mac[6]) {
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "nsetup: socket failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) {
        fprintf(stderr, "nsetup: SIOCGIFHWADDR failed for %s: %s\n", ifname, strerror(errno));
        close(fd);
        return -1;
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    close(fd);
    return 0;
}

static int set_iface_up(const char *ifname) {
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "nsetup: socket failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        fprintf(stderr, "nsetup: SIOCGIFFLAGS failed for %s: %s\n", ifname, strerror(errno));
        close(fd);
        return -1;
    }

    if ((ifr.ifr_flags & IFF_UP) == 0) {
        ifr.ifr_flags |= IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0) {
            fprintf(stderr, "nsetup: SIOCSIFFLAGS failed for %s: %s\n", ifname, strerror(errno));
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

static int set_iface_sockaddr(const char *ifname, int request, uint32_t addr) {
    int fd;
    struct ifreq ifr;
    struct sockaddr_in sin;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "nsetup: socket failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    memset(&sin, 0, sizeof(sin));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = addr;
    memcpy(&ifr.ifr_addr, &sin, sizeof(sin));

    if (ioctl(fd, request, &ifr) != 0) {
        fprintf(stderr, "nsetup: ioctl %d failed for %s: %s\n", request, ifname, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int add_route(const char *ifname, uint32_t dst, uint32_t gateway, uint32_t mask, int gateway_route) {
    int fd;
    struct rtentry route;
    struct sockaddr_in *sin;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "nsetup: socket failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&route, 0, sizeof(route));

    sin = (struct sockaddr_in *)&route.rt_dst;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = dst;

    sin = (struct sockaddr_in *)&route.rt_gateway;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = gateway;

    sin = (struct sockaddr_in *)&route.rt_genmask;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = mask;

    route.rt_flags = RTF_UP;
    if (gateway_route) {
        route.rt_flags |= RTF_GATEWAY;
    }
    route.rt_dev = (char *)ifname;

    if (ioctl(fd, SIOCADDRT, &route) != 0 && errno != EEXIST) {
        fprintf(stderr, "nsetup: SIOCADDRT failed for %s: %s\n", ifname, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static void dhcp_add_option(unsigned char *options, size_t *offset, unsigned char code, const void *data, unsigned char len) {
    options[(*offset)++] = code;
    options[(*offset)++] = len;
    memcpy(options + *offset, data, len);
    *offset += len;
}

static size_t build_dhcp_packet(
    dhcp_packet_t *packet,
    uint32_t xid,
    const unsigned char mac[6],
    int msg_type,
    uint32_t requested_ip,
    uint32_t server_id,
    const char *ifname
) {
    size_t opt = 0;
    unsigned char msg = (unsigned char)msg_type;
    unsigned char client_id[7];
    unsigned char param_req[] = {
        DHCP_OPTION_SUBNET_MASK,
        DHCP_OPTION_ROUTER,
        DHCP_OPTION_DNS,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_LEASE_TIME,
        DHCP_OPTION_SERVER_ID
    };

    memset(packet, 0, sizeof(*packet));
    packet->op = DHCP_OP_BOOTREQUEST;
    packet->htype = DHCP_HTYPE_ETHERNET;
    packet->hlen = DHCP_HLEN_ETHERNET;
    packet->xid = htonl(xid);
    packet->flags = htons(DHCP_FLAG_BROADCAST);
    memcpy(packet->chaddr, mac, 6);
    packet->cookie = htonl(DHCP_MAGIC_COOKIE);

    dhcp_add_option(packet->options, &opt, DHCP_OPTION_MESSAGE_TYPE, &msg, 1);

    client_id[0] = DHCP_HTYPE_ETHERNET;
    memcpy(client_id + 1, mac, 6);
    dhcp_add_option(packet->options, &opt, DHCP_OPTION_CLIENT_ID, client_id, sizeof(client_id));

    if (ifname != NULL && ifname[0] != '\0') {
        size_t host_len = strlen(ifname);

        if (host_len > 24) {
            host_len = 24;
        }
        dhcp_add_option(packet->options, &opt, DHCP_OPTION_HOSTNAME, ifname, (unsigned char)host_len);
    }

    dhcp_add_option(packet->options, &opt, DHCP_OPTION_PARAM_REQ_LIST, param_req, sizeof(param_req));

    if (msg_type == DHCPREQUEST) {
        dhcp_add_option(packet->options, &opt, DHCP_OPTION_REQUESTED_IP, &requested_ip, 4);
        dhcp_add_option(packet->options, &opt, DHCP_OPTION_SERVER_ID, &server_id, 4);
    }

    packet->options[opt++] = DHCP_OPTION_END;
    return sizeof(*packet) - sizeof(packet->options) + opt;
}

static int send_dhcp_packet(int sock, const dhcp_packet_t *packet, size_t packet_size) {
    struct sockaddr_in dst;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DHCP_SERVER_PORT);
    dst.sin_addr.s_addr = INADDR_BROADCAST;

    if (sendto(sock, packet, packet_size, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        fprintf(stderr, "nsetup: sendto failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void init_lease(dhcp_lease_t *lease) {
    memset(lease, 0, sizeof(*lease));
    lease->subnet_mask = htonl(0xFFFFFF00U);
}

static int parse_dhcp_options(const unsigned char *options, size_t len, int *msg_type, dhcp_lease_t *lease) {
    size_t i = 0;

    while (i < len) {
        unsigned char code = options[i++];
        unsigned char opt_len;

        if (code == DHCP_OPTION_END) {
            return 0;
        }
        if (code == 0) {
            continue;
        }
        if (i >= len) {
            break;
        }

        opt_len = options[i++];
        if (i + opt_len > len) {
            break;
        }

        switch (code) {
            case DHCP_OPTION_MESSAGE_TYPE:
                if (opt_len == 1) {
                    *msg_type = options[i];
                }
                break;
            case DHCP_OPTION_SUBNET_MASK:
                if (opt_len == 4) {
                    memcpy(&lease->subnet_mask, options + i, 4);
                }
                break;
            case DHCP_OPTION_ROUTER:
                lease->router_count = 0;
                while (lease->router_count < MAX_ROUTERS && (lease->router_count + 1) * 4 <= opt_len) {
                    memcpy(
                        &lease->routers[lease->router_count],
                        options + i + lease->router_count * 4,
                        4
                    );
                    lease->router_count++;
                }
                break;
            case DHCP_OPTION_DNS:
                lease->dns_count = 0;
                while (lease->dns_count < MAX_DNS_SERVERS && (lease->dns_count + 1) * 4 <= opt_len) {
                    memcpy(
                        &lease->dns[lease->dns_count],
                        options + i + lease->dns_count * 4,
                        4
                    );
                    lease->dns_count++;
                }
                break;
            case DHCP_OPTION_LEASE_TIME:
                if (opt_len == 4) {
                    memcpy(&lease->lease_time, options + i, 4);
                }
                break;
            case DHCP_OPTION_SERVER_ID:
                if (opt_len == 4) {
                    memcpy(&lease->server_id, options + i, 4);
                }
                break;
            case DHCP_OPTION_DOMAIN_NAME:
                if (opt_len > 0) {
                    size_t copy_len = opt_len;

                    if (copy_len >= sizeof(lease->domain)) {
                        copy_len = sizeof(lease->domain) - 1;
                    }
                    memcpy(lease->domain, options + i, copy_len);
                    lease->domain[copy_len] = '\0';
                }
                break;
        }

        i += opt_len;
    }

    return -1;
}

static int recv_dhcp_reply(
    int sock,
    uint32_t xid,
    const unsigned char mac[6],
    int timeout_ms,
    int want_type,
    dhcp_lease_t *lease
) {
    long long deadline_ms;

    deadline_ms = now_ms() + timeout_ms;

    while (1) {
        struct timeval tv;
        unsigned char buf[DHCP_PACKET_MAX];
        ssize_t received;
        int remaining_ms;
        dhcp_packet_t *packet;
        int msg_type = 0;

        remaining_ms = (int)(deadline_ms - now_ms());
        if (remaining_ms <= 0) {
            break;
        }

        tv.tv_sec = remaining_ms / 1000;
        tv.tv_usec = (remaining_ms % 1000) * 1000;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            fprintf(stderr, "nsetup: setsockopt(SO_RCVTIMEO) failed: %s\n", strerror(errno));
            return -1;
        }

        received = recv(sock, buf, sizeof(buf), 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            fprintf(stderr, "nsetup: recv failed: %s\n", strerror(errno));
            return -1;
        }

        if ((size_t)received < sizeof(*packet) - sizeof(packet->options)) {
            continue;
        }

        packet = (dhcp_packet_t *)buf;
        if (packet->op != DHCP_OP_BOOTREPLY) {
            continue;
        }
        if (ntohl(packet->xid) != xid) {
            continue;
        }
        if (memcmp(packet->chaddr, mac, 6) != 0) {
            continue;
        }
        if (ntohl(packet->cookie) != DHCP_MAGIC_COOKIE) {
            continue;
        }

        init_lease(lease);
        lease->yiaddr = packet->yiaddr;

        if (parse_dhcp_options(
                buf + (sizeof(*packet) - sizeof(packet->options)),
                (size_t)received - (sizeof(*packet) - sizeof(packet->options)),
                &msg_type,
                lease
            ) != 0) {
            continue;
        }

        if (msg_type == DHCPNAK) {
            fprintf(stderr, "nsetup: DHCP server rejected the request\n");
            return -1;
        }
        if (msg_type == want_type) {
            return 0;
        }
    }

    fprintf(stderr, "nsetup: DHCP timeout waiting for %s\n", want_type == DHCPOFFER ? "offer" : "ack");
    return -1;
}

static int open_dhcp_socket(const char *ifname) {
    int sock;
    int yes = 1;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        fprintf(stderr, "nsetup: socket failed: %s\n", strerror(errno));
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        fprintf(stderr, "nsetup: setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0) {
        fprintf(stderr, "nsetup: setsockopt(SO_BROADCAST) failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) != 0) {
        fprintf(stderr, "nsetup: SO_BINDTODEVICE failed for %s: %s\n", ifname, strerror(errno));
        close(sock);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DHCP_CLIENT_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "nsetup: bind port 68 failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

static int write_resolv_conf(const dhcp_lease_t *lease) {
    FILE *fp;
    int i;

    if (lease->dns_count <= 0) {
        return 0;
    }

    fp = fopen("/etc/resolv.conf", "w");
    if (fp == NULL) {
        fprintf(stderr, "nsetup: warning: cannot write /etc/resolv.conf: %s\n", strerror(errno));
        return 0;
    }

    if (lease->domain[0] != '\0') {
        fprintf(fp, "search %s\n", lease->domain);
    }

    for (i = 0; i < lease->dns_count; i++) {
        char dns_buf[INET_ADDRSTRLEN];

        ipv4_to_str(lease->dns[i], dns_buf, sizeof(dns_buf));
        fprintf(fp, "nameserver %s\n", dns_buf);
    }

    fclose(fp);
    return 0;
}

static int apply_lease(const char *ifname, const dhcp_lease_t *lease) {
    uint32_t ip_host;
    uint32_t mask_host;
    uint32_t network;
    uint32_t broadcast;

    if (set_iface_up(ifname) != 0) {
        return -1;
    }
    if (set_iface_sockaddr(ifname, SIOCSIFADDR, lease->yiaddr) != 0) {
        return -1;
    }
    if (set_iface_sockaddr(ifname, SIOCSIFNETMASK, lease->subnet_mask) != 0) {
        return -1;
    }

    ip_host = ntohl(lease->yiaddr);
    mask_host = ntohl(lease->subnet_mask);
    network = htonl(ip_host & mask_host);
    broadcast = htonl((ip_host & mask_host) | (~mask_host));

    if (set_iface_sockaddr(ifname, SIOCSIFBRDADDR, broadcast) != 0) {
        return -1;
    }
    if (add_route(ifname, network, 0, lease->subnet_mask, 0) != 0) {
        return -1;
    }
    if (lease->router_count > 0 && add_route(ifname, 0, lease->routers[0], 0, 1) != 0) {
        return -1;
    }
    if (write_resolv_conf(lease) != 0) {
        return -1;
    }

    return 0;
}

static void print_lease_summary(const char *ifname, const dhcp_lease_t *lease) {
    char ip_buf[INET_ADDRSTRLEN];
    char mask_buf[INET_ADDRSTRLEN];
    char gw_buf[INET_ADDRSTRLEN] = "-";
    int i;

    ipv4_to_str(lease->yiaddr, ip_buf, sizeof(ip_buf));
    ipv4_to_str(lease->subnet_mask, mask_buf, sizeof(mask_buf));

    if (lease->router_count > 0) {
        ipv4_to_str(lease->routers[0], gw_buf, sizeof(gw_buf));
    }

    printf("\nnetwork ready on %s\n", ifname);
    printf("  ip:      %s\n", ip_buf);
    printf("  netmask: %s\n", mask_buf);
    printf("  gateway: %s\n", gw_buf);

    if (lease->dns_count > 0) {
        printf("  dns:     ");
        for (i = 0; i < lease->dns_count; i++) {
            char dns_buf[INET_ADDRSTRLEN];

            ipv4_to_str(lease->dns[i], dns_buf, sizeof(dns_buf));
            printf("%s%s", i == 0 ? "" : ", ", dns_buf);
        }
        printf("\n");
    }
}

static int run_dhcp(const char *ifname, const unsigned char mac[6], dhcp_lease_t *lease) {
    dhcp_packet_t packet;
    uint32_t xid;
    int sock;
    size_t packet_size;

    xid = (uint32_t)(time(NULL) ^ getpid() ^ (mac[3] << 16) ^ (mac[4] << 8) ^ mac[5]);
    sock = open_dhcp_socket(ifname);
    if (sock < 0) {
        return -1;
    }

    packet_size = build_dhcp_packet(&packet, xid, mac, DHCPDISCOVER, 0, 0, ifname);
    if (send_dhcp_packet(sock, &packet, packet_size) != 0) {
        close(sock);
        return -1;
    }
    printf("waiting for DHCP offer...\n");
    if (recv_dhcp_reply(sock, xid, mac, DHCP_TIMEOUT_MS, DHCPOFFER, lease) != 0) {
        close(sock);
        return -1;
    }

    if (lease->server_id == 0 || lease->yiaddr == 0) {
        fprintf(stderr, "nsetup: incomplete DHCP offer\n");
        close(sock);
        return -1;
    }

    packet_size = build_dhcp_packet(&packet, xid, mac, DHCPREQUEST, lease->yiaddr, lease->server_id, ifname);
    if (send_dhcp_packet(sock, &packet, packet_size) != 0) {
        close(sock);
        return -1;
    }
    printf("waiting for DHCP ack...\n");
    if (recv_dhcp_reply(sock, xid, mac, DHCP_TIMEOUT_MS, DHCPACK, lease) != 0) {
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

int main(int argc, char *argv[]) {
    iface_info_t *ifaces = NULL;
    size_t iface_count = 0;
    char ifname[IFNAMSIZ];
    unsigned char mac[6];
    dhcp_lease_t lease;
    int iface_index;

    if (argc > 2) {
        fprintf(stderr, "nsetup: usage: nsetup [iface]\n");
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "nsetup: root is required\n");
        return 1;
    }
    if (collect_interfaces(&ifaces, &iface_count) != 0) {
        return 1;
    }
    if (iface_count == 0) {
        fprintf(stderr, "nsetup: no interfaces found\n");
        free(ifaces);
        return 1;
    }

    if (argc == 2 && argv[1][0] != '\0') {
        snprintf(ifname, sizeof(ifname), "%s", argv[1]);
    } else {
        print_interfaces(ifaces, iface_count);
        printf("\nselect interface: ");
        fflush(stdout);

        if (scanf("%15s", ifname) != 1) {
            fprintf(stderr, "nsetup: interface name expected\n");
            free(ifaces);
            return 1;
        }
    }

    iface_index = find_iface(ifaces, iface_count, ifname);
    if (iface_index < 0) {
        fprintf(stderr, "nsetup: unknown interface %s\n", ifname);
        free(ifaces);
        return 1;
    }
    if (ifaces[iface_index].flags & IFF_LOOPBACK) {
        fprintf(stderr, "nsetup: %s is loopback\n", ifname);
        free(ifaces);
        return 1;
    }
    if (get_iface_mac(ifname, mac) != 0) {
        free(ifaces);
        return 1;
    }
    if (set_iface_up(ifname) != 0) {
        free(ifaces);
        return 1;
    }
    if (run_dhcp(ifname, mac, &lease) != 0) {
        free(ifaces);
        return 1;
    }
    if (apply_lease(ifname, &lease) != 0) {
        free(ifaces);
        return 1;
    }

    print_lease_summary(ifname, &lease);
    free(ifaces);
    return 0;
}
