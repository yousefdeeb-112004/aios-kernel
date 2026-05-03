/* =============================================================================
 * Network Stack
 *
 * Layers:
 *   Ethernet (L2) — frame with MAC addresses
 *   ARP         — resolve IP to MAC address
 *   IP  (L3)    — packet routing
 *   UDP (L4)    — simple datagram protocol
 *
 * Shell commands (Arabic):
 *   shbk  (شبكة shabaka = network)  — network status
 *   irsl  (إرسال irsal = send)      — send UDP packet
 *   stlm  (استلام istilam = receive) — check received packets
 *   pci   — scan PCI bus devices
 * ============================================================================= */
#ifndef _KERNEL_NET_H
#define _KERNEL_NET_H

#include <kernel/types.h>

/* MAC address (6 bytes) */
typedef struct { uint8_t b[6]; } mac_addr_t;

/* IPv4 address (4 bytes) */
typedef struct { uint8_t b[4]; } ip_addr_t;

/* Ethernet frame header (14 bytes) */
typedef struct {
    mac_addr_t dst;
    mac_addr_t src;
    uint16_t   ethertype;   /* 0x0800=IP, 0x0806=ARP */
} __attribute__((packed)) eth_header_t;

#define ETH_TYPE_IP   0x0008   /* Big-endian 0x0800 */
#define ETH_TYPE_ARP  0x0608   /* Big-endian 0x0806 */

/* ARP packet (28 bytes) */
typedef struct {
    uint16_t hw_type;       /* 1 = Ethernet */
    uint16_t proto_type;    /* 0x0800 = IPv4 */
    uint8_t  hw_len;        /* 6 */
    uint8_t  proto_len;     /* 4 */
    uint16_t opcode;        /* 1=request, 2=reply */
    mac_addr_t sender_mac;
    ip_addr_t  sender_ip;
    mac_addr_t target_mac;
    ip_addr_t  target_ip;
} __attribute__((packed)) arp_packet_t;

#define ARP_REQUEST  0x0100  /* Big-endian 1 */
#define ARP_REPLY    0x0200  /* Big-endian 2 */

/* IPv4 header (20 bytes minimum) */
typedef struct {
    uint8_t  ver_ihl;       /* Version (4) + IHL (5) = 0x45 */
    uint8_t  tos;
    uint16_t total_len;     /* Big-endian */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;      /* 17 = UDP */
    uint16_t checksum;
    ip_addr_t src;
    ip_addr_t dst;
} __attribute__((packed)) ip_header_t;

#define IP_PROTO_UDP  17

/* UDP header (8 bytes) */
typedef struct {
    uint16_t src_port;      /* Big-endian */
    uint16_t dst_port;
    uint16_t length;        /* Header + data, big-endian */
    uint16_t checksum;      /* 0 = disabled */
} __attribute__((packed)) udp_header_t;

/* Network interface state */
typedef struct {
    bool       up;          /* Interface is active */
    mac_addr_t mac;         /* Our MAC address */
    ip_addr_t  ip;          /* Our IP (10.0.2.15 default for QEMU) */
    ip_addr_t  gateway;     /* Gateway (10.0.2.2 for QEMU) */
    ip_addr_t  netmask;     /* 255.255.255.0 */
    /* Stats */
    uint32_t   tx_packets;
    uint32_t   tx_bytes;
    uint32_t   rx_packets;
    uint32_t   rx_bytes;
    uint32_t   tx_errors;
    uint32_t   rx_errors;
} net_iface_t;

extern net_iface_t g_net;

/* Packet buffer */
#define NET_MTU 1536
typedef struct {
    uint8_t  data[NET_MTU];
    uint32_t length;
    bool     valid;
} net_packet_t;

/* Receive buffer (ring) */
#define NET_RX_RING  8
typedef struct {
    net_packet_t packets[NET_RX_RING];
    uint32_t write_idx;
    uint32_t read_idx;
    uint32_t count;
} net_rx_buffer_t;

extern net_rx_buffer_t g_net_rx;

/* Initialize networking (PCI scan + NIC driver + stack) */
void net_init(void);

/* Send a raw ethernet frame */
int32_t net_send_raw(const void* data, uint32_t len);

/* Send a UDP packet to ip:port */
int32_t net_send_udp(ip_addr_t dst_ip, uint16_t dst_port,
                     uint16_t src_port, const void* data, uint32_t len);

/* Check for received packets. Returns packet or NULL. */
net_packet_t* net_receive(void);

/* Process received packet (ARP reply, etc.) */
void net_process_rx(net_packet_t* pkt);

/* Byte-swap helpers */
static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* Consume a received packet after processing */
void net_rx_consume(void);

/* Process all pending packets (ARP, echo, DHCP) */
void net_poll(void);

/* === DHCP Client === */
#define DHCP_SERVER_PORT  67
#define DHCP_CLIENT_PORT  68
#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

typedef struct {
    uint8_t  op;            /* 1=request, 2=reply */
    uint8_t  htype;         /* 1=ethernet */
    uint8_t  hlen;          /* 6 */
    uint8_t  hops;
    uint32_t xid;           /* Transaction ID */
    uint16_t secs;
    uint16_t flags;         /* 0x8000 = broadcast */
    uint32_t ciaddr;        /* Client IP */
    uint32_t yiaddr;        /* Your (offered) IP */
    uint32_t siaddr;        /* Server IP */
    uint32_t giaddr;        /* Gateway IP */
    uint8_t  chaddr[16];    /* Client MAC + padding */
    uint8_t  sname[64];     /* Server hostname */
    uint8_t  file[128];     /* Boot filename */
    uint32_t magic;         /* DHCP magic cookie */
    uint8_t  options[308];  /* DHCP options */
} __attribute__((packed)) dhcp_packet_t;

/* DHCP result */
typedef struct {
    ip_addr_t ip;
    ip_addr_t subnet;
    ip_addr_t gateway;
    ip_addr_t dns;
    ip_addr_t server;
    uint32_t  lease_time;
    bool      success;
} dhcp_result_t;

extern dhcp_result_t g_dhcp;

/* Run full DHCP handshake (Discover→Offer→Request→Ack) */
int32_t net_dhcp_request(void);

/* === UDP Echo Server === */
typedef struct {
    bool     running;
    uint16_t port;
    uint32_t packets_echoed;
    uint32_t bytes_echoed;
    uint32_t errors;
} echo_server_t;

extern echo_server_t g_echo;

void net_echo_start(uint16_t port);
void net_echo_stop(void);

/* Network status / shell commands */
void net_dump(void);

/* === IP checksum (used by TCP too) === */
uint16_t ip_checksum(const void* data, uint32_t len);

/* === TCP === */
#define IP_PROTO_TCP  6

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   /* (offset << 4) | reserved */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_header_t;

/* TCP flags */
#define TCP_FIN   0x01
#define TCP_SYN   0x02
#define TCP_RST   0x04
#define TCP_PSH   0x08
#define TCP_ACK   0x10
#define TCP_URG   0x20

/* TCP connection state */
typedef enum {
    TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED,
    TCP_FIN_WAIT, TCP_CLOSE_WAIT, TCP_LAST_ACK
} tcp_state_t;

/* TCP connection */
#define TCP_MAX_CONNS  4
#define TCP_RX_BUF     2048

typedef struct {
    bool         used;
    tcp_state_t  state;
    ip_addr_t    remote_ip;
    uint16_t     local_port;
    uint16_t     remote_port;
    uint32_t     seq;          /* Our sequence number */
    uint32_t     ack;          /* Their sequence number */
    uint8_t      rx_buf[TCP_RX_BUF];
    uint32_t     rx_len;
    uint32_t     tx_bytes;
    uint32_t     rx_bytes;
} tcp_conn_t;

extern tcp_conn_t g_tcp_conns[TCP_MAX_CONNS];

/* TCP API */
int32_t tcp_connect(ip_addr_t ip, uint16_t port);          /* Returns conn index */
int32_t tcp_send(int32_t conn, const void* data, uint32_t len);
int32_t tcp_recv(int32_t conn, void* buf, uint32_t maxlen); /* Returns bytes received */
int32_t tcp_close(int32_t conn);
void    tcp_dump(void);

/* === DNS Resolution === */
#define DNS_PORT  53

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_header_t;

/* Resolve hostname to IP. Returns 0 on success. */
int32_t dns_resolve(const char* hostname, ip_addr_t* result);
void    dns_dump(const char* hostname);

#endif
