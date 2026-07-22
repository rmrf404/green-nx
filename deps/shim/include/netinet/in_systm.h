/* Minimal <netinet/in_systm.h> shim for Nintendo Switch (devkitA64/newlib). */
#ifndef _SWITCH_SHIM_NETINET_IN_SYSTM_H_
#define _SWITCH_SHIM_NETINET_IN_SYSTM_H_

#include <stdint.h>

typedef uint16_t n_short; /* short as received from the net */
typedef uint32_t n_long;  /* long as received from the net  */
typedef uint32_t n_time;  /* ms since 00:00 GMT, byte rev   */

#endif /* _SWITCH_SHIM_NETINET_IN_SYSTM_H_ */
