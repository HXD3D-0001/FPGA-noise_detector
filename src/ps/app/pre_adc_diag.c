#include "pre_adc_diag.h"
#include <string.h>
void pre_adc_diag_init(PreAdcDiag*d){if(d)memset(d,0,sizeof(*d));}
bool pre_adc_diag_offer(PreAdcDiag*d,const PreAdcDiagEvent*e){
 if(!d||!e)return false;
 if(d->count==PRE_ADC_DIAG_SLOTS){if(d->dropped!=UINT64_MAX)d->dropped++;return false;}
 d->events[(d->head+d->count)%PRE_ADC_DIAG_SLOTS]=*e;d->count++;
 if(d->count>d->highwater)d->highwater=d->count;
 return true;
}
bool pre_adc_diag_take(PreAdcDiag*d,PreAdcDiagEvent*e){
 if(!d||!e||!d->count)return false;
 *e=d->events[d->head];d->head=(d->head+1)%PRE_ADC_DIAG_SLOTS;d->count--;return true;
}
