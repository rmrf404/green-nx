/* Minimal <netinet/ip.h> shim for Nintendo Switch (devkitA64/newlib).
 * BSD-style IPv4 header definitions, enough for usrsctp's userspace stack. */
#ifndef _SWITCH_SHIM_NETINET_IP_H_
#define _SWITCH_SHIM_NETINET_IP_H_

#include <stdint.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>

/* AArch64 Switch is little-endian. */
struct ip {
    uint8_t ip_hl : 4, /* header length */
        ip_v : 4;      /* version */
    uint8_t ip_tos;    /* type of service */
    uint16_t ip_len;   /* total length */
    uint16_t ip_id;    /* identification */
    uint16_t ip_off;   /* fragment offset field */
#define IP_RF 0x8000   /* reserved fragment flag */
#define IP_DF 0x4000   /* don't fragment flag */
#define IP_MF 0x2000   /* more fragments flag */
#define IP_OFFMASK 0x1fff /* mask for fragmenting bits */
    uint8_t ip_ttl;    /* time to live */
    uint8_t ip_p;      /* protocol */
    uint16_t ip_sum;   /* checksum */
    struct in_addr ip_src, ip_dst; /* source and dest address */
} __attribute__((packed));

#define IPVERSION 4
#define IP_MAXPACKET 65535 /* maximum packet size */
#define IPDEFTTL 64        /* default ttl, from RFC 1340 */
#define MAXTTL 255

#define IPTOS_ECN_NOTECT 0x00
#define IPTOS_ECN_ECT1 0x01
#define IPTOS_ECN_ECT0 0x02
#define IPTOS_ECN_CE 0x03
#define IPTOS_ECN_MASK 0x03

#endif /* _SWITCH_SHIM_NETINET_IP_H_ */
