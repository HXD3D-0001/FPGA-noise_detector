#include "ps_smoke.h"
#include "xil_assert.h"

uint32_t __real_ps_smoke_self_test(void);

/* This pure generated-BSP function proves the genuine archive participates.
 * No vendor startup, UART, cache or PS initialization is called. Build only. */
uint32_t __wrap_ps_smoke_self_test(void) {
    XNullHandler((void *)0);
    return __real_ps_smoke_self_test();
}
