/*
 * dhcpd — DHCP-клиент CactOS (аналог dhclient/dhcpcd для Linux).
 *
 * Держит на сетевой карте адрес по DHCP: DISCOVER/OFFER/REQUEST/ACK,
 * продлевает аренду по T1/T2, применяет полученный конфиг (ip/mask/gw/dns)
 * в ядро через /dev/net CACT_NETCTL_NETCFG. Ядро DHCP не выполняет —
 * только применяет то, что ему скажет этот демон (или networkd).
 *
 * Использование:
 *   dhcpd [-i IFACE] [-f]
 *
 *   -i IFACE   имя интерфейса (по умолчанию eth0; карта в CactOS одна)
 *   -f         не уходить в фоновый режим (по умолчанию и так foreground)
 *
 * Демон рассчитан на запуск от root (нужен CACT_NETCTL_NETCFG).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <ioctl_abi.h>

#define DHCP_MAGIC      0x63825363u
#define DHCP_OPT_MSG_TYPE  53
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_REQ_IP    50
#define DHCP_OPT_SUBNET    1
#define DHCP_OPT_ROUTER    3
#define DHCP_OPT_DNS       6
#define DHCP_OPT_LEASE     51
#define DHCP_OPT_T1        58
#define DHCP_OPT_T2        59
#define DHCP_OPT_PARAM_REQ 55
#define DHCP_OPT_HOSTNAME  12
#define DHCP_OPT_END       255

#define DHCP_TYPE_DISCOVER 1
#define DHCP_TYPE_OFFER    2
#define DHCP_TYPE_REQUEST  3
#define DHCP_TYPE_ACK      5
#define DHCP_TYPE_NAK      6

#define DHCP_CLIENT_PORT   68
#define DHCP_SERVER_PORT   67

#define IFACE_DEFAULT "eth0"

#define LEASE_DEFAULT_S   3600u

struct dhcp_hdr {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  opts[312];
} __attribute__((packed));

typedef struct {
    uint32_t ip_host;      /* yiaddr от сервера, host order */
    uint32_t netmask_host;
    uint32_t gateway_host;
    uint32_t dns_host;
    uint32_t server_host;  /* option 54 */
    uint32_t lease_s;
    uint32_t t1_s;
    uint32_t t2_s;
    uint8_t  mac[6];
} lease_t;

static const char *g_iface = IFACE_DEFAULT;
static uint32_t    g_xid   = 0;

/* ────────────────────────────────────────────────────────────────────────── */

static int opt_get_u32(const uint8_t *opts, int opts_len, uint8_t key, uint32_t *out) {
    int i = 0;
    while (i < opts_len) {
        uint8_t t = opts[i++];
        if (t == DHCP_OPT_END) break;
        if (t == 0) continue;
        if (i >= opts_len) break;
        uint8_t l = opts[i++];
        if (i + l > opts_len) break;
        if (t == key && l >= 4) {
            /* Wire bytes are already the address in reading order, so the
             * big-endian assembly *is* the host-order value (a.b.c.d ->
             * 0xAABBCCDD) that ip_print()/net_apply() expect.  Swapping here
             * too would double-convert every option. */
            uint32_t netv = ((uint32_t)opts[i] << 24) |
                            ((uint32_t)opts[i + 1] << 16) |
                            ((uint32_t)opts[i + 2] << 8) |
                            (uint32_t)opts[i + 3];
            *out = netv;
            return 0;
        }
        i += l;
    }
    return -1;
}

static int opt_get_u8(const uint8_t *opts, int opts_len, uint8_t key, uint8_t *out) {
    int i = 0;
    while (i < opts_len) {
        uint8_t t = opts[i++];
        if (t == DHCP_OPT_END) break;
        if (t == 0) continue;
        if (i >= opts_len) break;
        uint8_t l = opts[i++];
        if (i + l > opts_len) break;
        if (t == key && l >= 1) {
            *out = opts[i];
            return 0;
        }
        i += l;
    }
    return -1;
}

static void opt_append_u8(uint8_t *opts, int *off, int cap, uint8_t key, uint8_t val) {
    if (*off + 3 > cap) return;
    opts[(*off)++] = key;
    opts[(*off)++] = 1;
    opts[(*off)++] = val;
}

static void opt_append_u32(uint8_t *opts, int *off, int cap, uint8_t key, uint32_t host_val) {
    if (*off + 6 > cap) return;
    /* Emit the host-order value big-endian: no htonl() first, or the two
     * conversions cancel and the address goes out byte-reversed. */
    opts[(*off)++] = key;
    opts[(*off)++] = 4;
    opts[(*off)++] = (uint8_t)((host_val >> 24) & 0xFF);
    opts[(*off)++] = (uint8_t)((host_val >> 16) & 0xFF);
    opts[(*off)++] = (uint8_t)((host_val >> 8) & 0xFF);
    opts[(*off)++] = (uint8_t)(host_val & 0xFF);
}

static void opt_end(uint8_t *opts, int *off, int cap) {
    if (*off < cap) opts[(*off)++] = DHCP_OPT_END;
}

/* ── /dev/net: чтение/применение конфига ядра ───────────────────────────── */

static int net_get(cact_netcfg_get_t *g) {
    int fd = open("/dev/net", O_RDWR);
    if (fd < 0) return -1;
    memset(g, 0, sizeof(*g));
    int r = ioctl(fd, CACT_NETCTL_NETCFG_GET, g);
    close(fd);
    return r;
}

static int net_apply(const lease_t *ls) {
    cact_netcfg_arg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ip_host      = ls->ip_host;
    cfg.netmask_host = ls->netmask_host;
    cfg.gateway_host = ls->gateway_host;
    cfg.dns_host     = ls->dns_host;

    int fd = open("/dev/net", O_RDWR);
    if (fd < 0) return -1;
    int r = ioctl(fd, CACT_NETCTL_NETCFG, &cfg);
    close(fd);
    return r;
}

/* ── сокет ───────────────────────────────────────────────────────────────── */

static int dhcp_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port   = htons(DHCP_CLIENT_PORT);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── сборка сообщений ───────────────────────────────────────────────────── */

static void dhcp_fill_hdr(struct dhcp_hdr *h, int op, const uint8_t mac[6]) {
    memset(h, 0, sizeof(*h));
    h->op    = (uint8_t)op;      /* 1 = BOOTREQUEST */
    h->htype = 1;                /* Ethernet */
    h->hlen  = 6;
    h->xid   = htonl(g_xid);
    h->flags = htons(0x8000);    /* broadcast reply */
    h->magic = htonl(DHCP_MAGIC);
    if (mac)
        memcpy(h->chaddr, mac, 6);
}

static void dhcp_build_discover(struct dhcp_hdr *h, const uint8_t mac[6]) {
    dhcp_fill_hdr(h, 1, mac);
    int oi = 0;
    opt_append_u8(h->opts, &oi, (int)sizeof(h->opts), DHCP_OPT_MSG_TYPE, DHCP_TYPE_DISCOVER);
    /* параметр-request list: subnet/router/dns/lease */
    h->opts[oi++] = DHCP_OPT_PARAM_REQ;
    h->opts[oi++] = 4;
    h->opts[oi++] = DHCP_OPT_SUBNET;
    h->opts[oi++] = DHCP_OPT_ROUTER;
    h->opts[oi++] = DHCP_OPT_DNS;
    h->opts[oi++] = DHCP_OPT_LEASE;
    opt_end(h->opts, &oi, (int)sizeof(h->opts));
}

/* type=3 (REQUEST): if we already have an address and server — RENEWING/REBINDING.
 * server_host != 0  -> unicast renew to server (REQ with ciaddr)
 * server_host == 0  -> broadcast REQUEST (SELECTING, with requested-ip) */
static void dhcp_build_request(struct dhcp_hdr *h, const uint8_t mac[6],
                               uint32_t requested_ip_h, uint32_t server_h,
                               uint32_t ciaddr_h, int include_reqip) {
    dhcp_fill_hdr(h, 1, mac);
    h->ciaddr = htonl(ciaddr_h);
    int oi = 0;
    opt_append_u8(h->opts, &oi, (int)sizeof(h->opts), DHCP_OPT_MSG_TYPE, DHCP_TYPE_REQUEST);
    if (include_reqip && requested_ip_h)
        opt_append_u32(h->opts, &oi, (int)sizeof(h->opts), DHCP_OPT_REQ_IP, requested_ip_h);
    if (server_h)
        opt_append_u32(h->opts, &oi, (int)sizeof(h->opts), DHCP_OPT_SERVER_ID, server_h);
    h->opts[oi++] = DHCP_OPT_PARAM_REQ;
    h->opts[oi++] = 4;
    h->opts[oi++] = DHCP_OPT_SUBNET;
    h->opts[oi++] = DHCP_OPT_ROUTER;
    h->opts[oi++] = DHCP_OPT_DNS;
    h->opts[oi++] = DHCP_OPT_LEASE;
    opt_end(h->opts, &oi, (int)sizeof(h->opts));
}

static int dhcp_sendto(int fd, const struct dhcp_hdr *h, uint32_t dst_ip_h, uint16_t dst_port_h) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(dst_port_h);
    dst.sin_addr   = htonl(dst_ip_h);
    return sendto(fd, h, sizeof(*h), 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* Ждём ответ с нужным типом до deadline (time()). */
static int dhcp_wait_reply(int fd, uint32_t want_type, time_t deadline,
                           struct dhcp_hdr *rep, lease_t *out) {
    while (time(0) < deadline) {
        struct sockaddr_in from;
        uint32_t fromlen = sizeof(from);
        int n = recvfrom(fd, rep, sizeof(*rep), 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            sleep(1);
            continue;
        }
        if ((size_t)n < offsetof(struct dhcp_hdr, opts) + 4) continue;
        if (rep->op != 2) continue;                       /* BOOTREPLY */
        if (rep->xid != htonl(g_xid)) continue;           /* чужой транзакции */
        if (ntohl(rep->magic) != DHCP_MAGIC) continue;

        int opts_len = n - (int)offsetof(struct dhcp_hdr, opts);
        uint8_t mt = 0;
        if (opt_get_u8(rep->opts, opts_len, DHCP_OPT_MSG_TYPE, &mt) < 0) continue;
        if (mt == DHCP_TYPE_NAK) return -2;               /* сервер отказал */
        if (mt != want_type) continue;

        out->ip_host = ntohl(rep->yiaddr);
        (void)opt_get_u32(rep->opts, opts_len, DHCP_OPT_SUBNET, &out->netmask_host);
        if (out->netmask_host == 0) out->netmask_host = 0xFFFFFF00u;
        (void)opt_get_u32(rep->opts, opts_len, DHCP_OPT_ROUTER, &out->gateway_host);
        (void)opt_get_u32(rep->opts, opts_len, DHCP_OPT_DNS, &out->dns_host);
        (void)opt_get_u32(rep->opts, opts_len, DHCP_OPT_SERVER_ID, &out->server_host);
        uint32_t lease = 0, t1 = 0, t2 = 0;
        (void)opt_get_u32(rep->opts, opts_len, DHCP_OPT_LEASE, &lease);
        (void)opt_get_u32(rep->opts, opts_len, DHCP_OPT_T1, &t1);
        (void)opt_get_u32(rep->opts, opts_len, DHCP_OPT_T2, &t2);
        out->lease_s = lease ? lease : LEASE_DEFAULT_S;
        out->t1_s    = t1 ? t1 : out->lease_s / 2;
        out->t2_s    = t2 ? t2 : (out->lease_s * 7) / 8;
        if (out->t1_s == 0 || out->t1_s > out->lease_s) out->t1_s = out->lease_s / 2;
        if (out->t2_s < out->t1_s || out->t2_s > out->lease_s) out->t2_s = out->lease_s;
        return 0;
    }
    return -1;   /* таймаут */
}

static void lease_copy_mac(lease_t *ls, const uint8_t mac[6]) {
    if (mac) memcpy(ls->mac, mac, 6);
}

static void ip_print(uint32_t ip_h) {
    printf("%u.%u.%u.%u",
           (unsigned)((ip_h >> 24) & 0xFF), (unsigned)((ip_h >> 16) & 0xFF),
           (unsigned)((ip_h >> 8) & 0xFF), (unsigned)(ip_h & 0xFF));
}

/* Полный цикл INIT/SELECTING/REQUESTING: DISCOVER->OFFER->REQUEST->ACK.
 * Возвращает 0 и заполненный lease при успехе. */
static int dhcp_acquire(int fd, const uint8_t mac[6], lease_t *out) {
    struct dhcp_hdr req, rep;

    int got_offer = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        g_xid = (uint32_t)((uint32_t)getpid() << 16) ^
                (uint32_t)time(0) ^ (uint32_t)(attempt + 1) ^ 0xCA7CD00Du;
        dhcp_build_discover(&req, mac);
        printf("dhcpd: discover #%d (xid=%08x)\n", attempt + 1, (unsigned)g_xid);
        if (dhcp_sendto(fd, &req, 0xFFFFFFFFu, DHCP_SERVER_PORT) < 0) {
            printf("dhcpd: discover send failed\n");
            sleep(2);
            continue;
        }
        time_t deadline = time(0) + (2 << attempt);   /* 2,4,8,16,32 c */
        int r = dhcp_wait_reply(fd, DHCP_TYPE_OFFER, deadline, &rep, out);
        if (r == 0) {
            got_offer = 1;
            break;
        }
        if (r == -2) { printf("dhcpd: NAK on discover\n"); return -1; }
    }
    if (!got_offer) {
        printf("dhcpd: no offer received\n");
        return -1;
    }

    printf("dhcpd: offer ip="); ip_print(out->ip_host);
    if (out->server_host) { printf(" server="); ip_print(out->server_host); }
    printf("\n");

    lease_copy_mac(out, mac);
    for (int attempt = 0; attempt < 5; attempt++) {
        /* SELECTING: REQUEST выбранного адреса, broadcast, include server id */
        dhcp_build_request(&req, mac, out->ip_host, out->server_host, 0, 1);
        printf("dhcpd: request #%d\n", attempt + 1);
        if (dhcp_sendto(fd, &req, 0xFFFFFFFFu, DHCP_SERVER_PORT) < 0) {
            sleep(2);
            continue;
        }
        time_t deadline = time(0) + (2 << attempt);
        int r = dhcp_wait_reply(fd, DHCP_TYPE_ACK, deadline, &rep, out);
        if (r == 0) return 0;       /* bound */
        if (r == -2) { printf("dhcpd: NAK on request\n"); return -1; }
    }
    printf("dhcpd: no ack received\n");
    return -1;
}

/* Продление аренды: RENEWING (unicast серверу) затем REBINDING (broadcast).
 * Возвращает 0 при продлении, -1 при неудаче (аренда истекла). */
static int dhcp_renew(int fd, const uint8_t mac[6], lease_t *ls) {
    struct dhcp_hdr req, rep;
    time_t now = time(0);

    lease_copy_mac(ls, mac);
    for (int attempt = 0; attempt < 4; attempt++) {
        g_xid = (uint32_t)((uint32_t)getpid() << 16) ^
                (uint32_t)(time(0) + attempt) ^ 0xD00DFEEDu;
        if (attempt < 2 && ls->server_host) {
            /* RENEWING: к серверу напрямую */
            dhcp_build_request(&req, mac, ls->ip_host, ls->server_host,
                               ls->ip_host, 0);
            printf("dhcpd: renew to server (unicast)\n");
            if (dhcp_sendto(fd, &req, ls->server_host, DHCP_SERVER_PORT) < 0) {
                sleep(2);
                continue;
            }
        } else {
            /* REBINDING: broadcast */
            dhcp_build_request(&req, mac, ls->ip_host, 0, ls->ip_host, 0);
            printf("dhcpd: rebind (broadcast)\n");
            if (dhcp_sendto(fd, &req, 0xFFFFFFFFu, DHCP_SERVER_PORT) < 0) {
                sleep(2);
                continue;
            }
        }
        time_t deadline = time(0) + (2 << attempt);
        int r = dhcp_wait_reply(fd, DHCP_TYPE_ACK, deadline, &rep, ls);
        if (r == 0) return 0;
        if (r == -2) return -1;
    }
    (void)now;
    return -1;
}

static void lease_print(const lease_t *ls) {
    printf("dhcpd: lease ip="); ip_print(ls->ip_host);
    printf(" mask="); ip_print(ls->netmask_host);
    if (ls->gateway_host) { printf(" gw="); ip_print(ls->gateway_host); }
    if (ls->dns_host)     { printf(" dns="); ip_print(ls->dns_host); }
    if (ls->server_host)  { printf(" server="); ip_print(ls->server_host); }
    printf(" lease=%us t1=%us t2=%us\n",
           (unsigned)ls->lease_s, (unsigned)ls->t1_s, (unsigned)ls->t2_s);
}

int main(int argc, char *argv[]) {
    int foreground = 1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        if (argv[i][1] == 'i' && i + 1 < argc) {
            g_iface = argv[++i];
        } else if (argv[i][1] == 'f') {
            foreground = 1;
        } else if (argv[i][1] == 'd') {
            foreground = 0;   /* «daemon»: подсказка, детач делаем вручную ниже */
        }
    }
    (void)foreground;

    for (;;) {
        /* ждём появления карты */
        cact_netcfg_get_t link;
        memset(&link, 0, sizeof(link));
        for (;;) {
            if (net_get(&link) == 0 && link.link_up && link.mac[0] != 0)
                break;
            sleep(1);
        }
        printf("dhcpd: link up, mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               link.mac[0], link.mac[1], link.mac[2],
               link.mac[3], link.mac[4], link.mac[5]);

        int fd = dhcp_socket();
        if (fd < 0) {
            printf("dhcpd: cannot open DHCP socket (bind :68)\n");
            sleep(3);
            continue;
        }

        lease_t ls;
        memset(&ls, 0, sizeof(ls));
        if (dhcp_acquire(fd, link.mac, &ls) != 0) {
            close(fd);
            sleep(5);
            continue;   /* сервера нет/сеть не готова — пробуем снова */
        }

        if (net_apply(&ls) != 0) {
            printf("dhcpd: failed to apply config (root needed?)\n");
            close(fd);
            sleep(3);
            continue;
        }
        lease_print(&ls);

        /* BOUND: спим до T1, затем renew; при неудаче — до T2 и полный перезапуск */
        int renewed = 0;
        time_t bound_at = time(0);
        while (1) {
            time_t renew_at = bound_at + (time_t)ls.t1_s;
            time_t dead_at  = bound_at + (time_t)ls.lease_s;
            while (time(0) < renew_at)
                sleep(5);

            printf("dhcpd: attempting renewal at T1\n");
            if (dhcp_renew(fd, link.mac, &ls) == 0) {
                if (net_apply(&ls) == 0)
                    lease_print(&ls);
                bound_at = time(0);
                renewed = 1;
                continue;
            }
            printf("dhcpd: renewal failed, will rebind until lease end\n");
            while (time(0) < dead_at)
                sleep(5);
            renewed = 0;
            break;
        }
        (void)renewed;
        close(fd);
        printf("dhcpd: lease cycle finished, restarting\n");
    }
    return 0;
}
