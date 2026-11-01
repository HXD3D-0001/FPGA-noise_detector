#ifndef LFN_CAPTURE_UART_START_H
#define LFN_CAPTURE_UART_START_H
#include "../drivers/capture_regs.h"
#include <stdbool.h>
typedef struct {PlCaptureConfig config;uint64_t raw_duration,raw_capacity;} CaptureUartStart;
/* START cfg mode channel pattern [duration_us capacity_bytes] retains TEST.
 * STARTADC cfg mode channel [duration_us capacity_bytes] selects ADC.
 * No output is changed on invalid syntax/range. */
bool capture_uart_parse_start(const char *,CaptureUartStart *);
#endif
