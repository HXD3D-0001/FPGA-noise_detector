#ifndef LFN_PRE_ADC_DIAG_H
#define LFN_PRE_ADC_DIAG_H
#include <stdint.h>
#include <stdbool.h>
enum { PRE_ADC_DIAG_SLOTS=16 };
typedef struct {
 uint64_t command_id,session_id,capture_id,snapshot_version;
 uint32_t config_id,state,result,raw_reason;int32_t failure;
} PreAdcDiagEvent;
/* Controller task owns both offer and take. A separate UART task must add
 * external serialization; this ring is deliberately not a volatile mailbox.
 * No allocation, formatting, IO, wait, or service energy dequeue occurs here. */
typedef struct {PreAdcDiagEvent events[PRE_ADC_DIAG_SLOTS];unsigned head,count,highwater;uint64_t dropped;} PreAdcDiag;
void pre_adc_diag_init(PreAdcDiag *);
bool pre_adc_diag_offer(PreAdcDiag *,const PreAdcDiagEvent *);
bool pre_adc_diag_take(PreAdcDiag *,PreAdcDiagEvent *);
#endif
