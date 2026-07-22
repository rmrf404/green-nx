/* glibc-style <endian.h> shim for Nintendo Switch (devkitA64/newlib).
 * newlib provides <sys/endian.h> with underscore-prefixed macros. */
#ifndef _SWITCH_SHIM_ENDIAN_H_
#define _SWITCH_SHIM_ENDIAN_H_

#include <sys/endian.h>

#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN _LITTLE_ENDIAN
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN _BIG_ENDIAN
#endif
#ifndef __PDP_ENDIAN
#define __PDP_ENDIAN _PDP_ENDIAN
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER _BYTE_ORDER
#endif

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN _LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN _BIG_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER _BYTE_ORDER
#endif

#endif /* _SWITCH_SHIM_ENDIAN_H_ */
