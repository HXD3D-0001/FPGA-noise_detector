#include "ps_smoke.h"

static volatile uint32_t *const mailbox=(volatile uint32_t *)UINT32_C(0x0002f000);
static void barrier(void) {__asm__ volatile("dsb sy" ::: "memory");}

void ps_smoke_main(void) {
    uint32_t sctlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    /* Preserve the host's nonce written after download and before continue. */
    for(unsigned i=0;i<PS_MB_WORDS;i++) if(i!=PS_MB_NONCE) mailbox[i]=0;
    mailbox[PS_MB_MAGIC]=PS_SMOKE_MAGIC;
    mailbox[PS_MB_VERSION]=PS_SMOKE_VERSION;
    mailbox[PS_MB_SCTLR]=sctlr;
    barrier();
    mailbox[PS_MB_STATUS]=PS_SMOKE_RUNNING;
    uint32_t completed_nonce=0;
    for(;;) {
        ++mailbox[PS_MB_HEARTBEAT];
        uint32_t nonce=mailbox[PS_MB_NONCE];
        if(nonce!=0 && nonce!=completed_nonce) {
            mailbox[PS_MB_STATUS]=PS_SMOKE_RUNNING;
            mailbox[PS_MB_CHECKS]=0;
            mailbox[PS_MB_RESPONSE]=0;
            mailbox[PS_MB_FAULT]=0;
            barrier();
            uint32_t checks=ps_smoke_self_test();
            mailbox[PS_MB_CHECKS]=checks;
            mailbox[PS_MB_RESPONSE]=ps_smoke_response(nonce);
            mailbox[PS_MB_FAULT]=(checks==PS_SMOKE_ALL_CHECKS) ? 0 : UINT32_C(0x200);
            barrier();
            mailbox[PS_MB_STATUS]=(checks==PS_SMOKE_ALL_CHECKS) ? PS_SMOKE_PASS : PS_SMOKE_FAIL;
            completed_nonce=nonce;
        }
        barrier();
    }
}
