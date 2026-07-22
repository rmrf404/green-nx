/* Force-included compatibility header for building BSD/POSIX networking code
 * (usrsctp) on Nintendo Switch (devkitA64/newlib + libnx).
 * Injected with: -include switch_missing.h */
#ifndef _SWITCH_SHIM_MISSING_H_
#define _SWITCH_SHIM_MISSING_H_

/* libnx declares htons/ntohs/htonl/ntohl only in <arpa/inet.h>;
 * BSD code expects them from <netinet/in.h>. */
#include <arpa/inet.h>
#include <sys/socket.h>

/* libnx <sys/socket.h> lacks CMSG_ALIGN (has CMSG_LEN/CMSG_SPACE). */
#ifndef CMSG_ALIGN
#ifdef _ALIGN
#define CMSG_ALIGN(n) _ALIGN(n)
#else
#define CMSG_ALIGN(n) (((n) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#endif
#endif

/* newlib has no ERESTART; usrsctp uses it as an internal pseudo-errno for
 * interrupted waits. Any distinct value not colliding with real errnos works. */
#include <errno.h>
#ifndef ERESTART
#define ERESTART 700
#endif

/* Maximum iovec count for scatter/gather (BSD/Linux default). */
#ifndef UIO_MAXIOV
#define UIO_MAXIOV 1024
#endif
#ifndef IOV_MAX
#define IOV_MAX UIO_MAXIOV
#endif

#endif /* _SWITCH_SHIM_MISSING_H_ */
