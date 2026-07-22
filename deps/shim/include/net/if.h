/* Minimal <net/if.h> shim for Nintendo Switch (devkitA64/newlib).
 * Only the pieces used by usrsctp / libpeer. Flag values follow BSD. */
#ifndef _SWITCH_SHIM_NET_IF_H_
#define _SWITCH_SHIM_NET_IF_H_

#include <sys/types.h>
#include <sys/socket.h>

#define IF_NAMESIZE 16
#ifndef IFNAMSIZ
#define IFNAMSIZ IF_NAMESIZE
#endif

#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_DEBUG 0x4
#define IFF_LOOPBACK 0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING 0x40
#define IFF_NOARP 0x80
#define IFF_PROMISC 0x100
#define IFF_ALLMULTI 0x200
#define IFF_MULTICAST 0x8000

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        short ifru_flags;
        int ifru_metric;
        int ifru_mtu;
        void* ifru_data;
    } ifr_ifru;
#define ifr_addr ifr_ifru.ifru_addr
#define ifr_dstaddr ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_flags ifr_ifru.ifru_flags
#define ifr_metric ifr_ifru.ifru_metric
#define ifr_mtu ifr_ifru.ifru_mtu
#define ifr_data ifr_ifru.ifru_data
};

#ifdef __cplusplus
extern "C" {
#endif

unsigned int if_nametoindex(const char* ifname);
char* if_indextoname(unsigned int ifindex, char* ifname);

#ifdef __cplusplus
}
#endif

#endif /* _SWITCH_SHIM_NET_IF_H_ */
