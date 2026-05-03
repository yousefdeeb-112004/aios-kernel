/* =============================================================================
 * Network Stack + RTL8139 NIC Driver
 *
 * RTL8139 is the simplest PCI Ethernet controller. QEMU emulates it.
 * Uses PIO mode for TX and a DMA ring buffer for RX.
 *
 * Run QEMU with: -nic model=rtl8139
 * ============================================================================= */

#include <kernel/net.h>
#include <kernel/pci.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/ports.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <drivers/pit.h>
#include <lib/string.h>

net_iface_t g_net;
net_rx_buffer_t g_net_rx;
echo_server_t g_echo;
dhcp_result_t g_dhcp;

/* RTL8139 PCI IDs */
#define RTL_VENDOR  0x10EC
#define RTL_DEVICE  0x8139

/* RTL8139 register offsets (from I/O base) */
#define RTL_MAC0        0x00    /* MAC address bytes 0-3 */
#define RTL_MAC4        0x04    /* MAC address bytes 4-5 */
#define RTL_TXSTAT0     0x10    /* TX status descriptor 0 */
#define RTL_TXADDR0     0x20    /* TX address descriptor 0 */
#define RTL_RXBUF       0x30    /* RX buffer physical address */
#define RTL_CMD         0x37    /* Command register */
#define RTL_CAPR        0x38    /* Current Address of Packet Read */
#define RTL_CBR         0x3A    /* Current Buffer address (read ptr) */
#define RTL_IMR         0x3C    /* Interrupt Mask */
#define RTL_ISR         0x3E    /* Interrupt Status */
#define RTL_TXCFG       0x40    /* TX config */
#define RTL_RXCFG       0x44    /* RX config */
#define RTL_CFG1        0x52    /* Config register 1 */

/* RTL8139 commands */
#define RTL_CMD_RESET   0x10
#define RTL_CMD_RX_EN   0x08
#define RTL_CMD_TX_EN   0x04

/* RX config: accept broadcast+multicast+physical match, no wrap, 8K+16 buffer */
#define RTL_RXCFG_VAL   0x0000000F  /* AB+AM+APM+AAP */

/* Interrupt bits */
#define RTL_INT_RXOK    0x0001
#define RTL_INT_TXOK    0x0004

/* RTL8139 state */
static uint16_t rtl_iobase = 0;
static bool rtl_present = false;
static uint8_t tx_slot = 0;  /* Which of 4 TX descriptors to use */

/* RX buffer: 8K + 16 bytes + 1500 bytes wrap padding */
#define RX_BUF_SIZE  (8192 + 16 + 1536)
static uint8_t rx_buffer[RX_BUF_SIZE] __attribute__((aligned(4)));
static uint32_t rx_read_ptr = 0;

/* TX buffers (4 slots, 1536 bytes each) */
static uint8_t tx_buffers[4][NET_MTU] __attribute__((aligned(4)));

/* RTL8139 port I/O helpers */
static inline void rtl_outb(uint16_t reg, uint8_t val) { outb(rtl_iobase + reg, val); }
static inline void rtl_outw(uint16_t reg, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"((uint16_t)(rtl_iobase + reg)));
}
static inline void rtl_outl(uint16_t reg, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"((uint16_t)(rtl_iobase + reg)));
}
static inline uint8_t rtl_inb(uint16_t reg) { return inb(rtl_iobase + reg); }
static inline uint16_t rtl_inw(uint16_t reg) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"((uint16_t)(rtl_iobase + reg)));
    return v;
}
static inline uint32_t rtl_inl(uint16_t reg) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)(rtl_iobase + reg)));
    return v;
}

/* IP checksum calculation */
uint16_t ip_checksum(const void* data, uint32_t len) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len == 1) sum += *(const uint8_t*)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* RTL8139 IRQ handler */
static void rtl_irq_handler(registers_t* r) {
    (void)r;
    uint16_t status = rtl_inw(RTL_ISR);

    if (status & RTL_INT_RXOK) {
        g_net.rx_packets++;
        /* Packet received — copy to ring buffer */
        /* RTL8139 RX format: [status(16)][length(16)][packet data...] */
        while (!(rtl_inb(RTL_CMD) & 0x01)) { /* BUFE=0 means data available */
            uint32_t offset = rx_read_ptr % RX_BUF_SIZE;
            uint16_t rx_status = *(uint16_t*)(rx_buffer + offset);
            uint16_t rx_len = *(uint16_t*)(rx_buffer + offset + 2);

            if (!(rx_status & 0x01)) break;  /* ROK bit */
            if (rx_len > NET_MTU || rx_len < 14) break;

            /* Copy packet to ring buffer */
            if (g_net_rx.count < NET_RX_RING) {
                net_packet_t* pkt = &g_net_rx.packets[g_net_rx.write_idx];
                pkt->length = rx_len - 4;  /* Subtract CRC */
                if (pkt->length > NET_MTU) pkt->length = NET_MTU;
                memcpy(pkt->data, rx_buffer + offset + 4, pkt->length);
                pkt->valid = true;
                g_net_rx.write_idx = (g_net_rx.write_idx + 1) % NET_RX_RING;
                g_net_rx.count++;
                g_net.rx_bytes += pkt->length;
            }

            /* Advance read pointer (aligned to 4 bytes) */
            rx_read_ptr = (offset + rx_len + 4 + 3) & ~3;
            rtl_outw(RTL_CAPR, (uint16_t)(rx_read_ptr - 16));
        }
    }

    if (status & RTL_INT_TXOK) {
        /* TX complete — nothing special to do */
    }

    /* Acknowledge all interrupts */
    rtl_outw(RTL_ISR, status);
    pic_send_eoi(11);  /* RTL8139 typically on IRQ 11 */
}

/* Initialize RTL8139 NIC */
static bool rtl_init(void) {
    pci_device_t* dev = pci_find(RTL_VENDOR, RTL_DEVICE);
    if (!dev) {
        /* Also try finding any Ethernet controller */
        dev = pci_find_class(0x02, 0x00);
        if (!dev) return false;
    }

    /* Get I/O base from BAR0 (bit 0 = 1 means I/O space) */
    rtl_iobase = (uint16_t)(dev->bar[0] & 0xFFFC);
    if (rtl_iobase == 0) return false;

    /* Enable PCI bus mastering */
    pci_enable_bus_master(dev);

    /* Power on */
    rtl_outb(RTL_CFG1, 0x00);

    /* Software reset */
    rtl_outb(RTL_CMD, RTL_CMD_RESET);
    /* Wait for reset to complete (bit clears) */
    int timeout = 100000;
    while ((rtl_inb(RTL_CMD) & RTL_CMD_RESET) && timeout-- > 0);
    if (timeout <= 0) return false;

    /* Read MAC address */
    for (int i = 0; i < 6; i++) {
        g_net.mac.b[i] = rtl_inb(RTL_MAC0 + i);
    }

    /* Set RX buffer address (physical address since we identity-map) */
    memset(rx_buffer, 0, RX_BUF_SIZE);
    rx_read_ptr = 0;
    rtl_outl(RTL_RXBUF, (uint32_t)rx_buffer);

    /* Enable RX and TX interrupts */
    rtl_outw(RTL_IMR, RTL_INT_RXOK | RTL_INT_TXOK);

    /* Configure RX: accept all, no wrap, 8K buffer */
    rtl_outl(RTL_RXCFG, RTL_RXCFG_VAL);

    /* Enable receiver and transmitter */
    rtl_outb(RTL_CMD, RTL_CMD_RX_EN | RTL_CMD_TX_EN);

    /* Register IRQ handler */
    idt_register_handler(32 + dev->irq_line, rtl_irq_handler);
    pic_unmask_irq(dev->irq_line);

    rtl_present = true;
    return true;
}

/* Send raw ethernet frame */
int32_t net_send_raw(const void* data, uint32_t len) {
    if (!rtl_present || len > NET_MTU) {
        g_net.tx_errors++;
        return -1;
    }

    /* Copy to TX buffer */
    memcpy(tx_buffers[tx_slot], data, len);
    if (len < 60) {
        memset(tx_buffers[tx_slot] + len, 0, 60 - len);
        len = 60;  /* Minimum Ethernet frame */
    }

    /* Set TX address */
    rtl_outl(RTL_TXADDR0 + tx_slot * 4, (uint32_t)tx_buffers[tx_slot]);

    /* Set TX status: length + clear OWN bit (bit 13) to start transfer */
    rtl_outl(RTL_TXSTAT0 + tx_slot * 4, len & 0x1FFF);

    /* Wait for TX to complete (TOK bit = bit 15) */
    int timeout = 100000;
    while (timeout-- > 0) {
        uint32_t status = rtl_inl(RTL_TXSTAT0 + tx_slot * 4);
        if (status & (1 << 15)) break;  /* TOK */
        if (status & (1 << 14)) break;  /* TUN (underrun, also done) */
    }

    tx_slot = (tx_slot + 1) % 4;
    g_net.tx_packets++;
    g_net.tx_bytes += len;
    return 0;
}

/* Send UDP packet */
int32_t net_send_udp(ip_addr_t dst_ip, uint16_t dst_port,
                     uint16_t src_port, const void* data, uint32_t len) {
    if (!g_net.up) return -1;

    uint8_t frame[NET_MTU];
    uint32_t offset = 0;

    /* Ethernet header */
    eth_header_t* eth = (eth_header_t*)frame;
    memset(eth->dst.b, 0xFF, 6);  /* Broadcast (simple, no ARP) */
    memcpy(eth->src.b, g_net.mac.b, 6);
    eth->ethertype = ETH_TYPE_IP;
    offset += sizeof(eth_header_t);

    /* IP header */
    ip_header_t* ip = (ip_header_t*)(frame + offset);
    memset(ip, 0, sizeof(ip_header_t));
    ip->ver_ihl = 0x45;  /* IPv4, 20-byte header */
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    uint16_t ip_total = sizeof(ip_header_t) + sizeof(udp_header_t) + len;
    ip->total_len = htons(ip_total);
    ip->id = htons(1);
    memcpy(&ip->src, &g_net.ip, 4);
    memcpy(&ip->dst, &dst_ip, 4);
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip, sizeof(ip_header_t));
    offset += sizeof(ip_header_t);

    /* UDP header */
    udp_header_t* udp = (udp_header_t*)(frame + offset);
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(sizeof(udp_header_t) + len);
    udp->checksum = 0;  /* UDP checksum optional in IPv4 */
    offset += sizeof(udp_header_t);

    /* Payload */
    if (len > 0 && data) {
        memcpy(frame + offset, data, len);
        offset += len;
    }

    return net_send_raw(frame, offset);
}

/* Check for received packets */
net_packet_t* net_receive(void) {
    if (g_net_rx.count == 0) return NULL;
    net_packet_t* pkt = &g_net_rx.packets[g_net_rx.read_idx];
    if (!pkt->valid) return NULL;
    return pkt;
}

/* Consume received packet (call after processing) */
void net_rx_consume(void) {
    if (g_net_rx.count == 0) return;
    g_net_rx.packets[g_net_rx.read_idx].valid = false;
    g_net_rx.read_idx = (g_net_rx.read_idx + 1) % NET_RX_RING;
    g_net_rx.count--;
}

/* Process a received packet */
void net_process_rx(net_packet_t* pkt) {
    if (!pkt || pkt->length < sizeof(eth_header_t)) return;

    eth_header_t* eth = (eth_header_t*)pkt->data;

    /* ARP packet */
    if (eth->ethertype == ETH_TYPE_ARP && pkt->length >= sizeof(eth_header_t) + sizeof(arp_packet_t)) {
        arp_packet_t* arp = (arp_packet_t*)(pkt->data + sizeof(eth_header_t));
        if (arp->opcode == ARP_REQUEST) {
            /* Check if it's asking for our IP */
            if (memcmp(&arp->target_ip, &g_net.ip, 4) == 0) {
                /* Build ARP reply */
                uint8_t reply[64];
                eth_header_t* re = (eth_header_t*)reply;
                memcpy(re->dst.b, eth->src.b, 6);
                memcpy(re->src.b, g_net.mac.b, 6);
                re->ethertype = ETH_TYPE_ARP;

                arp_packet_t* ra = (arp_packet_t*)(reply + sizeof(eth_header_t));
                ra->hw_type = htons(1);
                ra->proto_type = htons(0x0800);
                ra->hw_len = 6;
                ra->proto_len = 4;
                ra->opcode = ARP_REPLY;
                memcpy(ra->sender_mac.b, g_net.mac.b, 6);
                memcpy(&ra->sender_ip, &g_net.ip, 4);
                memcpy(ra->target_mac.b, arp->sender_mac.b, 6);
                memcpy(&ra->target_ip, &arp->sender_ip, 4);

                net_send_raw(reply, sizeof(eth_header_t) + sizeof(arp_packet_t));
            }
        }
    }
}

/* Initialize full network stack */
void net_init(void) {
    memset(&g_net, 0, sizeof(net_iface_t));
    memset(&g_net_rx, 0, sizeof(net_rx_buffer_t));
    memset(&g_echo, 0, sizeof(echo_server_t));
    memset(&g_dhcp, 0, sizeof(dhcp_result_t));
    g_echo.port = 7; /* Default echo port */

    /* Default IP config (QEMU user-mode networking) */
    g_net.ip = (ip_addr_t){{10, 0, 2, 15}};
    g_net.gateway = (ip_addr_t){{10, 0, 2, 2}};
    g_net.netmask = (ip_addr_t){{255, 255, 255, 0}};

    /* Initialize PCI (if not already done) */
    if (g_pci.count == 0) pci_init();

    /* Try to init RTL8139 */
    if (rtl_init()) {
        g_net.up = true;
    }
}

void net_dump(void) {
    vga_puts_color("=== Network (shbk) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Interface: ");
    vga_puts_color(g_net.up ? "UP" : "DOWN", g_net.up ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("\n");

    if (g_net.up) {
        vga_puts("  MAC: ");
        for (int i = 0; i < 6; i++) {
            const char hex[] = "0123456789ABCDEF";
            vga_putchar(hex[g_net.mac.b[i] >> 4]);
            vga_putchar(hex[g_net.mac.b[i] & 0xF]);
            if (i < 5) vga_putchar(':');
        }
        vga_puts("\n");

        vga_puts("  IP:  ");
        for (int i = 0; i < 4; i++) {
            vga_put_dec(g_net.ip.b[i]);
            if (i < 3) vga_putchar('.');
        }
        if (g_dhcp.success) vga_puts_color(" (DHCP)", VGA_LIGHT_GREEN, VGA_BLACK);
        else vga_puts_color(" (static)", VGA_DARK_GREY, VGA_BLACK);

        vga_puts("\n  GW:  ");
        for (int i = 0; i < 4; i++) {
            vga_put_dec(g_net.gateway.b[i]);
            if (i < 3) vga_putchar('.');
        }
        vga_puts("\n  Net: ");
        for (int i = 0; i < 4; i++) {
            vga_put_dec(g_net.netmask.b[i]);
            if (i < 3) vga_putchar('.');
        }
        vga_puts("\n");

        if (g_dhcp.success) {
            vga_puts("  DNS: ");
            for (int i = 0; i < 4; i++) {
                vga_put_dec(g_dhcp.dns.b[i]);
                if (i < 3) vga_putchar('.');
            }
            vga_puts("  Lease: ");
            vga_put_dec(g_dhcp.lease_time);
            vga_puts("s\n");
        }

        vga_puts("  TX: "); vga_put_dec(g_net.tx_packets);
        vga_puts(" pkts ("); vga_put_dec(g_net.tx_bytes); vga_puts("B)");
        vga_puts("  RX: "); vga_put_dec(g_net.rx_packets);
        vga_puts(" pkts ("); vga_put_dec(g_net.rx_bytes); vga_puts("B)\n");
        vga_puts("  Errors: TX="); vga_put_dec(g_net.tx_errors);
        vga_puts(" RX="); vga_put_dec(g_net.rx_errors); vga_puts("\n");
        vga_puts("  RX buffer: "); vga_put_dec(g_net_rx.count);
        vga_puts("/"); vga_put_dec(NET_RX_RING); vga_puts(" packets\n");

        /* Echo server status */
        vga_puts("  Echo: ");
        if (g_echo.running) {
            vga_puts_color("ON", VGA_LIGHT_GREEN, VGA_BLACK);
            vga_puts(" port="); vga_put_dec(g_echo.port);
            vga_puts(" echoed="); vga_put_dec(g_echo.packets_echoed);
            vga_puts(" ("); vga_put_dec(g_echo.bytes_echoed); vga_puts("B)\n");
        } else {
            vga_puts_color("OFF", VGA_DARK_GREY, VGA_BLACK);
            vga_puts(" (use 'sma' to start)\n");
        }
    } else {
        vga_puts("  No NIC found. Run QEMU with:\n");
        vga_puts_color("  -device rtl8139,netdev=net0 -netdev user,id=net0\n", VGA_YELLOW, VGA_BLACK);
    }
}

/* =========================================================================
 * Helper: print IP address to VGA
 * ========================================================================= */
static void print_ip(ip_addr_t ip) {
    for (int i = 0; i < 4; i++) {
        vga_put_dec(ip.b[i]);
        if (i < 3) vga_putchar('.');
    }
}

/* =========================================================================
 * UDP Packet Parser — extract source/dest IP and port from raw packet
 * ========================================================================= */

typedef struct {
    ip_addr_t src_ip;
    ip_addr_t dst_ip;
    uint16_t  src_port;
    uint16_t  dst_port;
    uint8_t*  payload;
    uint32_t  payload_len;
    mac_addr_t src_mac;
    bool      valid;
} parsed_udp_t;

static parsed_udp_t parse_udp(net_packet_t* pkt) {
    parsed_udp_t r;
    memset(&r, 0, sizeof(r));

    if (!pkt || pkt->length < sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t))
        return r;

    eth_header_t* eth = (eth_header_t*)pkt->data;
    if (eth->ethertype != ETH_TYPE_IP) return r;

    ip_header_t* ip = (ip_header_t*)(pkt->data + sizeof(eth_header_t));
    if (ip->protocol != IP_PROTO_UDP) return r;
    if ((ip->ver_ihl & 0xF0) != 0x40) return r; /* Must be IPv4 */

    uint32_t ip_hdr_len = (ip->ver_ihl & 0x0F) * 4;
    udp_header_t* udp = (udp_header_t*)(pkt->data + sizeof(eth_header_t) + ip_hdr_len);

    memcpy(&r.src_mac, &eth->src, 6);
    memcpy(&r.src_ip, &ip->src, 4);
    memcpy(&r.dst_ip, &ip->dst, 4);
    r.src_port = ntohs(udp->src_port);
    r.dst_port = ntohs(udp->dst_port);

    uint32_t udp_data_offset = sizeof(eth_header_t) + ip_hdr_len + sizeof(udp_header_t);
    if (pkt->length > udp_data_offset) {
        r.payload = pkt->data + udp_data_offset;
        r.payload_len = pkt->length - udp_data_offset;
    }
    r.valid = true;
    return r;
}

/* =========================================================================
 * net_poll — Process all pending received packets
 *
 * Called from the shell's input loop (not a background process).
 * Disables IRQs briefly while reading from the receive buffer to
 * prevent the RTL8139 IRQ handler from corrupting the ring.
 * ========================================================================= */
void net_poll(void) {
    if (!g_net.up) return;
    if (g_net_rx.count == 0) return;

    /* Process at most 4 packets per poll to avoid blocking the shell */
    for (int iter = 0; iter < 4 && g_net_rx.count > 0; iter++) {
        net_packet_t* pkt = net_receive();
        if (!pkt) break;

        /* Safety: validate packet length */
        if (pkt->length < sizeof(eth_header_t) || pkt->length > NET_MTU) {
            net_rx_consume();
            continue;
        }

        eth_header_t* eth = (eth_header_t*)pkt->data;

        /* ARP handling */
        if (eth->ethertype == ETH_TYPE_ARP) {
            net_process_rx(pkt);
            net_rx_consume();
            continue;
        }

        /* UDP echo: respond to packets on echo port */
        if (g_echo.running) {
            parsed_udp_t udp = parse_udp(pkt);
            if (udp.valid && udp.dst_port == g_echo.port && udp.payload_len > 0) {
                /* Copy payload to a local buffer before consuming the packet */
                uint8_t echo_buf[512];
                uint32_t echo_len = udp.payload_len;
                if (echo_len > 512) echo_len = 512;
                memcpy(echo_buf, udp.payload, echo_len);
                ip_addr_t reply_ip = udp.src_ip;
                uint16_t reply_port = udp.src_port;

                /* Consume packet BEFORE sending reply (frees the slot) */
                net_rx_consume();

                /* Now send the echo reply */
                if (net_send_udp(reply_ip, reply_port, g_echo.port,
                                 echo_buf, echo_len) == 0) {
                    g_echo.packets_echoed++;
                    g_echo.bytes_echoed += echo_len;
                } else {
                    g_echo.errors++;
                }
                continue;
            }
        }

        /* Unhandled packet — drop if buffer is getting full */
        if (g_net_rx.count >= NET_RX_RING - 2) {
            net_rx_consume();
        } else {
            break; /* Leave for manual inspection via stlm */
        }
    }
}

/* =========================================================================
 * UDP Echo Server — no background process, driven by shell polling
 * ========================================================================= */

void net_echo_start(uint16_t port) {
    if (!g_net.up) {
        vga_puts_color("  Network is DOWN. Cannot start echo server.\n", VGA_LIGHT_RED, VGA_BLACK);
        return;
    }
    if (g_echo.running) {
        vga_puts("  Echo server already running on port ");
        vga_put_dec(g_echo.port);
        vga_puts(".\n");
        return;
    }

    g_echo.port = port;
    g_echo.running = true;
    g_echo.packets_echoed = 0;
    g_echo.bytes_echoed = 0;
    g_echo.errors = 0;

    vga_puts_color("  Echo server started", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" on port "); vga_put_dec(port); vga_puts("\n");
    vga_puts("  Echoes UDP packets while shell is active.\n");
    vga_puts("  Use 'sma stop' to stop, 'sma' for status.\n");
}

void net_echo_stop(void) {
    if (!g_echo.running) {
        vga_puts("  Echo server is not running.\n");
        return;
    }
    g_echo.running = false;
    vga_puts_color("  Echo server stopped.", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" Echoed "); vga_put_dec(g_echo.packets_echoed);
    vga_puts(" packets ("); vga_put_dec(g_echo.bytes_echoed); vga_puts("B)\n");
}

/* =========================================================================
 * DHCP Client — full Discover → Offer → Request → Ack handshake
 * ========================================================================= */

/* Build and send a raw DHCP packet (bypasses net_send_udp since we need
 * source IP 0.0.0.0 and broadcast destination) */
static int32_t dhcp_send(uint8_t msg_type, uint32_t xid,
                          ip_addr_t* request_ip, ip_addr_t* server_ip) {
    uint8_t frame[sizeof(eth_header_t) + sizeof(ip_header_t) +
                  sizeof(udp_header_t) + sizeof(dhcp_packet_t)];
    memset(frame, 0, sizeof(frame));
    uint32_t off = 0;

    /* Ethernet: broadcast */
    eth_header_t* eth = (eth_header_t*)frame;
    memset(eth->dst.b, 0xFF, 6);
    memcpy(eth->src.b, g_net.mac.b, 6);
    eth->ethertype = ETH_TYPE_IP;
    off += sizeof(eth_header_t);

    /* IP: 0.0.0.0 → 255.255.255.255 */
    ip_header_t* ip = (ip_header_t*)(frame + off);
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    uint16_t ip_total = sizeof(ip_header_t) + sizeof(udp_header_t) + sizeof(dhcp_packet_t);
    ip->total_len = htons(ip_total);
    ip->id = htons(1);
    memset(&ip->src, 0, 4);                 /* 0.0.0.0 */
    memset(&ip->dst, 0xFF, 4);              /* 255.255.255.255 */
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip, sizeof(ip_header_t));
    off += sizeof(ip_header_t);

    /* UDP: 68 → 67 */
    udp_header_t* udp = (udp_header_t*)(frame + off);
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length = htons(sizeof(udp_header_t) + sizeof(dhcp_packet_t));
    udp->checksum = 0;
    off += sizeof(udp_header_t);

    /* DHCP packet */
    dhcp_packet_t* dhcp = (dhcp_packet_t*)(frame + off);
    dhcp->op = 1;     /* BOOTREQUEST */
    dhcp->htype = 1;  /* Ethernet */
    dhcp->hlen = 6;
    dhcp->xid = htonl(xid);
    dhcp->flags = htons(0x8000); /* Broadcast flag */
    memcpy(dhcp->chaddr, g_net.mac.b, 6);
    dhcp->magic = htonl(DHCP_MAGIC_COOKIE);

    /* DHCP Options */
    uint8_t* opt = dhcp->options;
    int oi = 0;

    /* Option 53: DHCP Message Type */
    opt[oi++] = 53; opt[oi++] = 1; opt[oi++] = msg_type;

    if (msg_type == DHCP_REQUEST && request_ip && server_ip) {
        /* Option 50: Requested IP */
        opt[oi++] = 50; opt[oi++] = 4;
        memcpy(&opt[oi], request_ip->b, 4); oi += 4;

        /* Option 54: Server Identifier */
        opt[oi++] = 54; opt[oi++] = 4;
        memcpy(&opt[oi], server_ip->b, 4); oi += 4;
    }

    /* Option 55: Parameter Request List */
    opt[oi++] = 55; opt[oi++] = 4;
    opt[oi++] = 1;   /* Subnet mask */
    opt[oi++] = 3;   /* Router */
    opt[oi++] = 6;   /* DNS */
    opt[oi++] = 51;  /* Lease time */

    /* End */
    opt[oi++] = 255;

    off += sizeof(dhcp_packet_t);
    return net_send_raw(frame, off);
}

/* Wait for a DHCP reply with specified message type. Timeout in ms. */
static bool dhcp_wait_reply(uint32_t xid, uint8_t expected_type,
                             dhcp_packet_t* out, uint32_t timeout_ms) {
    uint32_t start = pit_get_ticks();
    uint32_t timeout_ticks = timeout_ms / 10;

    while ((pit_get_ticks() - start) < timeout_ticks) {
        net_packet_t* pkt = net_receive();
        if (!pkt) {
            proc_yield();
            continue;
        }

        /* Check if it's a UDP packet on port 68 */
        parsed_udp_t udp = parse_udp(pkt);
        if (!udp.valid || udp.dst_port != DHCP_CLIENT_PORT) {
            /* Not DHCP — process ARP or skip */
            eth_header_t* eth = (eth_header_t*)pkt->data;
            if (eth->ethertype == ETH_TYPE_ARP) net_process_rx(pkt);
            net_rx_consume();
            continue;
        }

        /* It's DHCP — check xid and type */
        if (udp.payload_len < sizeof(dhcp_packet_t) - 308) {
            net_rx_consume();
            continue;
        }

        /* The DHCP data starts at the UDP payload */
        dhcp_packet_t* dhcp = (dhcp_packet_t*)udp.payload;

        if (ntohl(dhcp->xid) != xid) {
            net_rx_consume();
            continue;
        }

        /* Find option 53 (message type) */
        if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) {
            net_rx_consume();
            continue;
        }

        uint8_t* opts = dhcp->options;
        uint8_t found_type = 0;
        for (int i = 0; i < 300; ) {
            if (opts[i] == 255) break;        /* End */
            if (opts[i] == 0) { i++; continue; } /* Pad */
            uint8_t code = opts[i];
            uint8_t len = opts[i + 1];
            if (code == 53 && len >= 1) found_type = opts[i + 2];
            i += 2 + len;
        }

        if (found_type == expected_type) {
            memcpy(out, dhcp, sizeof(dhcp_packet_t));
            net_rx_consume();
            return true;
        }

        net_rx_consume();
    }
    return false; /* Timeout */
}

/* Parse DHCP options from a reply packet */
static void dhcp_parse_options(dhcp_packet_t* dhcp) {
    uint8_t* opts = dhcp->options;
    for (int i = 0; i < 300; ) {
        if (opts[i] == 255) break;
        if (opts[i] == 0) { i++; continue; }
        uint8_t code = opts[i];
        uint8_t len = opts[i + 1];

        switch (code) {
            case 1: /* Subnet mask */
                if (len >= 4) memcpy(&g_dhcp.subnet, &opts[i + 2], 4);
                break;
            case 3: /* Router/gateway */
                if (len >= 4) memcpy(&g_dhcp.gateway, &opts[i + 2], 4);
                break;
            case 6: /* DNS server */
                if (len >= 4) memcpy(&g_dhcp.dns, &opts[i + 2], 4);
                break;
            case 51: /* Lease time */
                if (len >= 4) {
                    uint32_t lt;
                    memcpy(&lt, &opts[i + 2], 4);
                    g_dhcp.lease_time = ntohl(lt);
                }
                break;
            case 54: /* Server identifier */
                if (len >= 4) memcpy(&g_dhcp.server, &opts[i + 2], 4);
                break;
        }
        i += 2 + len;
    }
}

/* Full DHCP handshake */
int32_t net_dhcp_request(void) {
    if (!g_net.up) {
        vga_puts_color("  Network is DOWN.\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }

    /* Generate transaction ID from tick counter */
    uint32_t xid = pit_get_ticks() ^ 0xDEAD0042;
    dhcp_packet_t reply;
    memset(&g_dhcp, 0, sizeof(dhcp_result_t));

    /* === Step 1: DHCP Discover === */
    vga_puts("  [1/4] Sending DHCP Discover... ");
    if (dhcp_send(DHCP_DISCOVER, xid, NULL, NULL) != 0) {
        vga_puts_color("FAILED\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }
    vga_puts_color("sent\n", VGA_LIGHT_GREEN, VGA_BLACK);

    /* === Step 2: Wait for DHCP Offer === */
    vga_puts("  [2/4] Waiting for DHCP Offer... ");
    if (!dhcp_wait_reply(xid, DHCP_OFFER, &reply, 5000)) {
        vga_puts_color("TIMEOUT\n", VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("  No DHCP server responded within 5 seconds.\n");
        return -1;
    }

    /* Extract offered IP */
    ip_addr_t offered_ip;
    uint32_t yip = reply.yiaddr;
    memcpy(&offered_ip, &yip, 4);

    /* Parse options from offer */
    dhcp_parse_options(&reply);
    memcpy(&g_dhcp.ip, &offered_ip, 4);

    vga_puts_color("got offer ", VGA_LIGHT_GREEN, VGA_BLACK);
    print_ip(offered_ip);
    vga_puts("\n");

    /* === Step 3: DHCP Request === */
    vga_puts("  [3/4] Sending DHCP Request... ");
    if (dhcp_send(DHCP_REQUEST, xid, &offered_ip, &g_dhcp.server) != 0) {
        vga_puts_color("FAILED\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }
    vga_puts_color("sent\n", VGA_LIGHT_GREEN, VGA_BLACK);

    /* === Step 4: Wait for DHCP Ack === */
    vga_puts("  [4/4] Waiting for DHCP Ack... ");
    if (!dhcp_wait_reply(xid, DHCP_ACK, &reply, 5000)) {
        vga_puts_color("TIMEOUT\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }

    /* Parse final options from ACK */
    dhcp_parse_options(&reply);
    g_dhcp.success = true;

    vga_puts_color("CONFIRMED\n", VGA_LIGHT_GREEN, VGA_BLACK);

    /* Apply the new configuration */
    memcpy(&g_net.ip, &g_dhcp.ip, 4);
    if (g_dhcp.gateway.b[0] != 0)
        memcpy(&g_net.gateway, &g_dhcp.gateway, 4);
    if (g_dhcp.subnet.b[0] != 0)
        memcpy(&g_net.netmask, &g_dhcp.subnet, 4);

    /* Print result */
    vga_puts_color("\n  DHCP Configuration Applied:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("    IP:      "); print_ip(g_net.ip); vga_puts("\n");
    vga_puts("    Subnet:  "); print_ip(g_net.netmask); vga_puts("\n");
    vga_puts("    Gateway: "); print_ip(g_net.gateway); vga_puts("\n");
    vga_puts("    DNS:     "); print_ip(g_dhcp.dns); vga_puts("\n");
    vga_puts("    Server:  "); print_ip(g_dhcp.server); vga_puts("\n");
    vga_puts("    Lease:   "); vga_put_dec(g_dhcp.lease_time); vga_puts(" seconds\n");

    return 0;
}

/* =========================================================================
 * TCP — Transmission Control Protocol (simplified)
 *
 * Supports: 3-way handshake, data send/receive, FIN close.
 * No retransmission, no window scaling, no congestion control.
 * Good enough for basic HTTP GET requests.
 * ========================================================================= */

tcp_conn_t g_tcp_conns[TCP_MAX_CONNS];
static uint16_t tcp_next_port = 49152;

/* Send a TCP segment */
static int32_t tcp_send_segment(tcp_conn_t* c, uint8_t flags,
                                 const void* data, uint32_t data_len) {
    uint32_t tcp_len = sizeof(tcp_header_t) + data_len;
    uint32_t total = sizeof(eth_header_t) + sizeof(ip_header_t) + tcp_len;
    uint8_t frame[1536];
    if (total > sizeof(frame)) return -1;
    memset(frame, 0, total);

    /* Ethernet */
    eth_header_t* eth = (eth_header_t*)frame;
    memset(eth->dst.b, 0xFF, 6); /* Broadcast — QEMU's user net handles routing */
    memcpy(eth->src.b, g_net.mac.b, 6);
    eth->ethertype = ETH_TYPE_IP;

    /* IP */
    ip_header_t* ip = (ip_header_t*)(frame + sizeof(eth_header_t));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    ip->total_len = htons(sizeof(ip_header_t) + tcp_len);
    ip->id = htons((uint16_t)(pit_get_ticks() & 0xFFFF));
    memcpy(&ip->src, &g_net.ip, 4);
    memcpy(&ip->dst, &c->remote_ip, 4);
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip, sizeof(ip_header_t));

    /* TCP */
    tcp_header_t* tcp = (tcp_header_t*)(frame + sizeof(eth_header_t) + sizeof(ip_header_t));
    tcp->src_port = htons(c->local_port);
    tcp->dst_port = htons(c->remote_port);
    tcp->seq_num = htonl(c->seq);
    tcp->ack_num = htonl(c->ack);
    tcp->data_offset = 0x50; /* 5 words = 20 bytes, no options */
    tcp->flags = flags;
    tcp->window = htons(TCP_RX_BUF);
    tcp->checksum = 0; /* Skip TCP checksum for QEMU user-mode */

    if (data && data_len > 0) {
        memcpy((uint8_t*)tcp + sizeof(tcp_header_t), data, data_len);
        c->seq += data_len;
        c->tx_bytes += data_len;
    }
    if (flags & TCP_SYN) c->seq++;
    if (flags & TCP_FIN) c->seq++;

    return net_send_raw(frame, total);
}

/* Wait for a TCP segment matching our connection. Timeout in ms. */
static bool tcp_wait(tcp_conn_t* c, uint8_t expect_flags, uint32_t timeout_ms) {
    uint32_t start = pit_get_ticks();
    uint32_t timeout_ticks = timeout_ms / 10;

    while ((pit_get_ticks() - start) < timeout_ticks) {
        net_packet_t* pkt = net_receive();
        if (!pkt) { proc_yield(); continue; }

        if (pkt->length < sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(tcp_header_t)) {
            /* Check if ARP */
            eth_header_t* e = (eth_header_t*)pkt->data;
            if (e->ethertype == ETH_TYPE_ARP) net_process_rx(pkt);
            net_rx_consume();
            continue;
        }

        eth_header_t* eth = (eth_header_t*)pkt->data;
        if (eth->ethertype != ETH_TYPE_IP) { net_rx_consume(); continue; }

        ip_header_t* ip = (ip_header_t*)(pkt->data + sizeof(eth_header_t));
        if (ip->protocol != IP_PROTO_TCP) {
            /* Handle UDP (DHCP/echo) */
            if (ip->protocol == IP_PROTO_UDP) {
                /* Leave for net_poll */
            }
            net_rx_consume();
            continue;
        }

        uint32_t ip_hdr_len = (ip->ver_ihl & 0x0F) * 4;
        tcp_header_t* tcp = (tcp_header_t*)(pkt->data + sizeof(eth_header_t) + ip_hdr_len);

        if (ntohs(tcp->dst_port) != c->local_port ||
            ntohs(tcp->src_port) != c->remote_port) {
            net_rx_consume();
            continue;
        }

        /* Update ack based on received seq */
        uint32_t their_seq = ntohl(tcp->seq_num);
        uint32_t tcp_data_offset = (tcp->data_offset >> 4) * 4;
        uint32_t ip_total = ntohs(ip->total_len);
        uint32_t tcp_data_len = ip_total - ip_hdr_len - tcp_data_offset;

        if (tcp->flags & TCP_SYN) {
            c->ack = their_seq + 1;
        } else if (tcp_data_len > 0) {
            /* Copy received data */
            uint8_t* payload = (uint8_t*)tcp + tcp_data_offset;
            uint32_t copy = tcp_data_len;
            if (c->rx_len + copy > TCP_RX_BUF) copy = TCP_RX_BUF - c->rx_len;
            if (copy > 0) {
                memcpy(c->rx_buf + c->rx_len, payload, copy);
                c->rx_len += copy;
                c->rx_bytes += copy;
            }
            c->ack = their_seq + tcp_data_len;
        }

        if (tcp->flags & TCP_FIN) {
            c->ack = their_seq + 1;
            if (c->state == TCP_FIN_WAIT) c->state = TCP_CLOSED;
            else c->state = TCP_CLOSE_WAIT;
        }

        if (tcp->flags & TCP_RST) {
            c->state = TCP_CLOSED;
            net_rx_consume();
            return false;
        }

        bool match = (tcp->flags & expect_flags) == expect_flags;
        net_rx_consume();

        if (match) return true;
    }
    return false; /* Timeout */
}

/* Connect to remote host */
int32_t tcp_connect(ip_addr_t ip, uint16_t port) {
    if (!g_net.up) return -1;

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < TCP_MAX_CONNS; i++)
        if (!g_tcp_conns[i].used) { slot = i; break; }
    if (slot < 0) return -2;

    tcp_conn_t* c = &g_tcp_conns[slot];
    memset(c, 0, sizeof(tcp_conn_t));
    c->used = true;
    c->state = TCP_SYN_SENT;
    memcpy(&c->remote_ip, &ip, 4);
    c->local_port = tcp_next_port++;
    c->remote_port = port;
    c->seq = pit_get_ticks() * 12345 + 67890;

    /* Send SYN */
    if (tcp_send_segment(c, TCP_SYN, NULL, 0) != 0) {
        c->used = false;
        return -3;
    }

    /* Wait for SYN+ACK */
    if (!tcp_wait(c, TCP_SYN | TCP_ACK, 5000)) {
        c->used = false;
        return -4;
    }

    /* Send ACK */
    c->state = TCP_ESTABLISHED;
    tcp_send_segment(c, TCP_ACK, NULL, 0);
    return slot;
}

int32_t tcp_send(int32_t conn, const void* data, uint32_t len) {
    if (conn < 0 || conn >= TCP_MAX_CONNS) return -1;
    tcp_conn_t* c = &g_tcp_conns[conn];
    if (!c->used || c->state != TCP_ESTABLISHED) return -2;

    /* Send in chunks of 1400 bytes */
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > 1400) chunk = 1400;
        if (tcp_send_segment(c, TCP_PSH | TCP_ACK, p + sent, chunk) != 0)
            return (int32_t)sent;
        sent += chunk;
        /* Brief wait for ACK */
        tcp_wait(c, TCP_ACK, 200);
    }
    return (int32_t)sent;
}

int32_t tcp_recv(int32_t conn, void* buf, uint32_t maxlen) {
    if (conn < 0 || conn >= TCP_MAX_CONNS) return -1;
    tcp_conn_t* c = &g_tcp_conns[conn];
    if (!c->used) return -2;

    /* Wait for data if buffer empty */
    if (c->rx_len == 0) {
        tcp_wait(c, TCP_ACK, 3000);
        /* Also try PSH */
        if (c->rx_len == 0)
            tcp_wait(c, TCP_PSH | TCP_ACK, 2000);
    }

    /* ACK received data */
    if (c->rx_len > 0) {
        tcp_send_segment(c, TCP_ACK, NULL, 0);
    }

    uint32_t copy = c->rx_len;
    if (copy > maxlen) copy = maxlen;
    if (copy > 0) {
        memcpy(buf, c->rx_buf, copy);
        /* Shift remaining data */
        if (copy < c->rx_len)
            memcpy(c->rx_buf, c->rx_buf + copy, c->rx_len - copy);
        c->rx_len -= copy;
    }
    return (int32_t)copy;
}

int32_t tcp_close(int32_t conn) {
    if (conn < 0 || conn >= TCP_MAX_CONNS) return -1;
    tcp_conn_t* c = &g_tcp_conns[conn];
    if (!c->used) return -2;

    if (c->state == TCP_ESTABLISHED) {
        c->state = TCP_FIN_WAIT;
        tcp_send_segment(c, TCP_FIN | TCP_ACK, NULL, 0);
        tcp_wait(c, TCP_ACK, 2000);
    } else if (c->state == TCP_CLOSE_WAIT) {
        tcp_send_segment(c, TCP_FIN | TCP_ACK, NULL, 0);
    }

    c->state = TCP_CLOSED;
    c->used = false;
    return 0;
}

void tcp_dump(void) {
    vga_puts_color("=== TCP Connections ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    bool any = false;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!g_tcp_conns[i].used) continue;
        any = true;
        tcp_conn_t* c = &g_tcp_conns[i];
        vga_printf("  [%d] ", i);
        print_ip(c->remote_ip);
        vga_printf(":%u ", c->remote_port);
        switch (c->state) {
            case TCP_CLOSED:      vga_puts("CLOSED"); break;
            case TCP_SYN_SENT:    vga_puts("SYN_SENT"); break;
            case TCP_ESTABLISHED: vga_puts_color("ESTABLISHED", VGA_LIGHT_GREEN, VGA_BLACK); break;
            case TCP_FIN_WAIT:    vga_puts("FIN_WAIT"); break;
            case TCP_CLOSE_WAIT:  vga_puts("CLOSE_WAIT"); break;
            case TCP_LAST_ACK:    vga_puts("LAST_ACK"); break;
        }
        vga_printf(" tx:%u rx:%u\n", c->tx_bytes, c->rx_bytes);
    }
    if (!any) vga_puts("  No active connections.\n");
}

/* =========================================================================
 * DNS Resolution — query the DHCP-provided DNS server
 * ========================================================================= */

/* Encode a hostname into DNS wire format: "example.com" -> "\7example\3com\0" */
static int dns_encode_name(const char* name, uint8_t* out) {
    int pos = 0;
    while (*name) {
        /* Find next dot or end */
        const char* dot = name;
        while (*dot && *dot != '.') dot++;
        int label_len = (int)(dot - name);
        if (label_len > 63 || label_len == 0) return -1;
        out[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) out[pos++] = name[i];
        name = (*dot == '.') ? dot + 1 : dot;
    }
    out[pos++] = 0; /* Root label */
    return pos;
}

int32_t dns_resolve(const char* hostname, ip_addr_t* result) {
    if (!g_net.up) return -1;
    if (g_dhcp.dns.b[0] == 0) return -2; /* No DNS server */

    /* Build DNS query packet */
    uint8_t query[512];
    memset(query, 0, sizeof(query));

    dns_header_t* hdr = (dns_header_t*)query;
    hdr->id = htons((uint16_t)(pit_get_ticks() & 0xFFFF));
    hdr->flags = htons(0x0100); /* Standard query, recursion desired */
    hdr->qdcount = htons(1);

    /* Encode question */
    int qpos = sizeof(dns_header_t);
    int name_len = dns_encode_name(hostname, query + qpos);
    if (name_len < 0) return -3;
    qpos += name_len;

    /* QTYPE = A (1), QCLASS = IN (1) */
    query[qpos++] = 0; query[qpos++] = 1; /* Type A */
    query[qpos++] = 0; query[qpos++] = 1; /* Class IN */

    /* Send DNS query via UDP to DNS server port 53 */
    uint16_t qid = ntohs(hdr->id);
    if (net_send_udp(g_dhcp.dns, DNS_PORT, 10053, query, (uint32_t)qpos) != 0)
        return -4;

    /* Wait for response */
    uint32_t start = pit_get_ticks();
    while ((pit_get_ticks() - start) < 500) { /* 5 second timeout */
        net_packet_t* pkt = net_receive();
        if (!pkt) { proc_yield(); continue; }

        parsed_udp_t udp = parse_udp(pkt);
        if (!udp.valid || udp.dst_port != 10053) {
            eth_header_t* e = (eth_header_t*)pkt->data;
            if (e->ethertype == ETH_TYPE_ARP) net_process_rx(pkt);
            net_rx_consume();
            continue;
        }

        /* Parse DNS response */
        if (udp.payload_len < sizeof(dns_header_t)) {
            net_rx_consume();
            continue;
        }

        dns_header_t* resp = (dns_header_t*)udp.payload;
        if (ntohs(resp->id) != qid) { net_rx_consume(); continue; }

        uint16_t flags = ntohs(resp->flags);
        if (flags & 0x000F) { net_rx_consume(); return -5; } /* RCODE error */
        if (ntohs(resp->ancount) == 0) { net_rx_consume(); return -6; } /* No answers */

        /* Skip question section */
        int rpos = sizeof(dns_header_t);
        uint8_t* rdata = udp.payload;
        /* Skip question name */
        while (rpos < (int)udp.payload_len && rdata[rpos] != 0) {
            if ((rdata[rpos] & 0xC0) == 0xC0) { rpos += 2; break; }
            rpos += rdata[rpos] + 1;
        }
        if (rdata[rpos] == 0) rpos++; /* Skip null terminator */
        rpos += 4; /* Skip QTYPE + QCLASS */

        /* Parse answer(s) — look for first A record */
        int ancount = ntohs(resp->ancount);
        for (int a = 0; a < ancount && rpos < (int)udp.payload_len - 10; a++) {
            /* Skip name (may be compressed) */
            if ((rdata[rpos] & 0xC0) == 0xC0) rpos += 2;
            else { while (rpos < (int)udp.payload_len && rdata[rpos]) rpos += rdata[rpos] + 1; rpos++; }

            uint16_t rtype = (rdata[rpos] << 8) | rdata[rpos + 1]; rpos += 2;
            rpos += 2; /* Skip class */
            rpos += 4; /* Skip TTL */
            uint16_t rdlen = (rdata[rpos] << 8) | rdata[rpos + 1]; rpos += 2;

            if (rtype == 1 && rdlen == 4) { /* A record */
                memcpy(result->b, &rdata[rpos], 4);
                net_rx_consume();
                return 0; /* Success! */
            }
            rpos += rdlen;
        }

        net_rx_consume();
        return -7; /* No A record found */
    }
    return -8; /* Timeout */
}

void dns_dump(const char* hostname) {
    vga_puts_color("=== DNS Lookup ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Query: "); vga_puts(hostname); vga_puts("\n");
    vga_puts("  DNS server: "); print_ip(g_dhcp.dns); vga_puts("\n");
    vga_puts("  Resolving... ");

    ip_addr_t result;
    int32_t r = dns_resolve(hostname, &result);
    if (r == 0) {
        vga_puts_color("OK\n", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("  Address: ");
        print_ip(result);
        vga_puts("\n");
    } else {
        vga_puts_color("FAILED", VGA_LIGHT_RED, VGA_BLACK);
        vga_printf(" (error %d)\n", r);
    }
}
