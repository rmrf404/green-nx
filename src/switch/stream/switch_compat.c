/* Symbols the WebRTC deps expect the Switch build to provide. The shim
 * headers in deps/shim/include declare these (and note they used to live in a
 * switch_compat.c inside libpeer's tree that never shipped with the repo);
 * defining them app-side resolves them at the final link just the same, so a
 * clean clone builds without hand-edits to deps/src.
 *
 *  - peer_log / gnx_peer_log_set: libpeer's LOG_REDIRECT sink (see
 *    deps/build-switch.sh, -DLOG_REDIRECT=1) routed to stream::Engine's log.
 *  - getifaddrs/freeifaddrs, if_nametoindex/if_indextoname: newlib/libnx has
 *    none; libpeer's ICE host-candidate scan and usrsctp's init want them.
 *  - mbedtls_hardware_poll: mbedtls is built with MBEDTLS_ENTROPY_HARDWARE_ALT
 *    (no /dev/urandom on HOS); back it with the system csrng.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__

#include <switch.h>
#include <unistd.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* ---- libpeer LOG_REDIRECT sink ------------------------------------------ */

static void (*g_peer_log_cb)(const char* line);

void gnx_peer_log_set(void (*cb)(const char* line)) { g_peer_log_cb = cb; }

void peer_log(char* level_tag, const char* file, int line, const char* fmt,
              ...) {
    if (!g_peer_log_cb) return;
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char full[512];
    snprintf(full, sizeof(full), "%s %s:%d %s", level_tag ? level_tag : "?",
             file ? file : "?", line, msg);
    g_peer_log_cb(full);
}

/* ---- ifaddrs ------------------------------------------------------------- */

/* One AF_INET entry ("wlan0") holding the console's current IP from libnx
 * gethostid(); that is all libpeer's host-candidate gathering needs. A single
 * malloc block keeps freeifaddrs trivial. */

struct gnx_ifaddrs_block {
    struct ifaddrs ifa;
    struct sockaddr_in addr;
    struct sockaddr_in netmask;
    char name[8];
};

int getifaddrs(struct ifaddrs** ifap) {
    if (!ifap) return -1;
    struct gnx_ifaddrs_block* block = calloc(1, sizeof(*block));
    if (!block) return -1;

    strcpy(block->name, "wlan0");
    block->addr.sin_family = AF_INET;
    block->addr.sin_addr.s_addr = (u32)gethostid();
    block->netmask.sin_family = AF_INET;
    block->netmask.sin_addr.s_addr = htonl(0xFFFFFF00);  /* /24 */

    block->ifa.ifa_next = NULL;
    block->ifa.ifa_name = block->name;
    block->ifa.ifa_flags = IFF_UP | IFF_RUNNING;
    block->ifa.ifa_addr = (struct sockaddr*)&block->addr;
    block->ifa.ifa_netmask = (struct sockaddr*)&block->netmask;

    *ifap = &block->ifa;
    return 0;
}

void freeifaddrs(struct ifaddrs* ifa) {
    while (ifa) {
        struct ifaddrs* next = ifa->ifa_next;
        free(ifa);  /* ifa is the first member of its gnx_ifaddrs_block */
        ifa = next;
    }
}

unsigned int if_nametoindex(const char* ifname) {
    (void)ifname;
    return 1;
}

char* if_indextoname(unsigned int ifindex, char* ifname) {
    (void)ifindex;
    if (!ifname) return NULL;
    strcpy(ifname, "wlan0");
    return ifname;
}

/* ---- mbedtls hardware entropy ------------------------------------------- */

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len,
                          size_t* olen) {
    (void)data;
    randomGet(output, len);
    if (olen) *olen = len;
    return 0;
}

#endif /* __SWITCH__ */
