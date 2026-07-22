/* Minimal <sys/uio.h> shim for Nintendo Switch (devkitA64/newlib + libnx).
 * newlib on Switch has no sys/uio.h; struct iovec lives in libnx's
 * <sys/_iovec.h>. usrsctp only needs the struct definition (it does its
 * own scatter/gather in userspace and never calls readv/writev). */
#ifndef _SWITCH_SHIM_SYS_UIO_H_
#define _SWITCH_SHIM_SYS_UIO_H_

#include <sys/types.h>
#include <sys/_iovec.h>

#endif /* _SWITCH_SHIM_SYS_UIO_H_ */
