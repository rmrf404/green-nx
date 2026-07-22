/* Minimal <ifaddrs.h> shim for Nintendo Switch (devkitA64/newlib + libnx).
 * getifaddrs() is implemented in libpeer's src/switch_compat.c: it returns a
 * single AF_INET entry ("wlan0") holding the console's current IP address
 * obtained from libnx gethostid(). */
#ifndef _SWITCH_SHIM_IFADDRS_H_
#define _SWITCH_SHIM_IFADDRS_H_

#include <sys/socket.h>

struct ifaddrs {
    struct ifaddrs* ifa_next;
    char* ifa_name;
    unsigned int ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr* ifa_dstaddr;
    void* ifa_data;
};
#define ifa_broadaddr ifa_dstaddr

#ifdef __cplusplus
extern "C" {
#endif

int getifaddrs(struct ifaddrs** ifap);
void freeifaddrs(struct ifaddrs* ifa);

#ifdef __cplusplus
}
#endif

#endif /* _SWITCH_SHIM_IFADDRS_H_ */
