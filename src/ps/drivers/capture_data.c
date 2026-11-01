#include "capture_data.h"
#include <string.h>
static int valid(const PlRegs*r){return r&&r->read32&&r->write32;
 }
static uint32_t rd(const PlRegs*r,uint32_t a){return r->read32(r->context,a);
 }
static uint64_t pair(const PlRegs*r,uint32_t a){uint64_t v=rd(r,a);
 return v|((uint64_t)rd(r,a+4)<<32);
 }
static bool same(PlDataAction a,PlDataAction b){return a.seq==b.seq&&a.mask==b.mask&&a.reset==b.reset&&a.resume==b.resume;
 }
PlResult pl_data_probe(const PlRegs*r){if(!valid(r))return PL_INVALID;
 return rd(r,0)==0x31544144&&rd(r,4)==0x10000?PL_OK:PL_REJECTED;
 }
PlResult pl_data_request(const PlRegs*r,uint32_t kind,uint32_t*out){
 PlResult p;
 uint32_t before,seq;
 
 if(!out||kind<1||kind>4)return PL_INVALID;
 
 p=pl_data_probe(r);
 
 if(p!=PL_OK)return p;
 
 if((rd(r,0xc)|rd(r,0x14))&1)return PL_BUSY;
 
 before=rd(r,0x10);
 
 if(before==UINT32_MAX)return PL_REJECTED;
 
 if(r->write32(r->context,8,kind))return PL_BUS;
 
 seq=rd(r,0x10);
 
 if(seq!=before+1)return PL_STALE;
 *out=seq;
 return PL_OK;
 
}
PlResult pl_data_poll(const PlRegs*r,uint32_t seq,uint32_t*out){
 PlResult p;
 uint32_t s;
 
 if(!seq)return PL_INVALID;
 p=pl_data_probe(r);
 
 if(p!=PL_OK)return p;
 
 if(rd(r,0x10)!=seq)return PL_STALE;
 
 s=rd(r,0xc);
 
 if(rd(r,0x10)!=seq)return PL_STALE;
 
 if(out)*out=s;
 
 if(s&8)return PL_TIMEOUT;
 
 if(s&1)return PL_BUSY;
 
 if(s&4)return PL_REJECTED;
 
 return s&2?PL_OK:PL_STALE;
 
}
PlResult pl_data_action(const PlRegs*r,PlDataAction*out){
 PlResult p;
 PlDataAction a;
 uint32_t v,again;
 
 if(!out)return PL_INVALID;
 p=pl_data_probe(r);
 
 if(p!=PL_OK)return p;
 
 a.seq=rd(r,0x18);
 v=rd(r,0x1c);
 again=rd(r,0x18);
 
 if(a.seq!=again||rd(r,0x1c)!=v)return PL_STALE;
 
 a.mask=v&3;
 a.reset=(v&0x100)!=0;
 a.resume=(v&0x200)!=0;
 
 if(v&~0x303u)return PL_INVALID;
 
 if(!a.mask)return PL_EMPTY;
 
 if(!a.seq||(a.resume&&(!a.reset||a.mask==3))||(!a.reset&&a.mask!=3))return PL_INVALID;
 
 *out=a;
 return PL_OK;
 
}
PlResult pl_data_ack(const PlRegs*r,const PlDataAction*a){
 PlDataAction current;
 PlResult p;
 
 if(!a)return PL_INVALID;
 p=pl_data_action(r,&current);
 
 if(p!=PL_OK)return p;
 
 if(!same(*a,current))return PL_STALE;
 
 if(r->write32(r->context,0x20,a->seq)||r->write32(r->context,0x24,a->mask))return PL_BUS;
 
 p=pl_data_action(r,&current);
 
 if(p!=PL_OK||!same(*a,current))return PL_STALE;
 
 if(r->write32(r->context,0x28,1))return PL_BUS;
 
 return PL_OK;
 
}
PlResult pl_data_energy_read(const PlRegs*r,PlEnergy*out){
 PlEnergy e;
 PlResult p;
 
 if(!out)return PL_INVALID;
 p=pl_data_probe(r);
 
 if(p!=PL_OK)return p;
 
 if(!rd(r,0x7c))return PL_EMPTY;
 
 if(r->write32(r->context,0x40,1))return PL_BUS;
 
 if(!(rd(r,0x78)&1))return PL_STALE;
 
 e.seq=pair(r,0x50);
 e.capture_id=pair(r,0x58);
 e.first_raw=pair(r,0x60);
 e.energy=pair(r,0x68);
 e.config_id=rd(r,0x70);
 e.flags=rd(r,0x74);
 
 if(!(rd(r,0x78)&1)||e.seq!=pair(r,0x50))return PL_STALE;
 
 if(r->write32(r->context,0x48,(uint32_t)e.seq)||r->write32(r->context,0x4c,(uint32_t)(e.seq>>32))||r->write32(r->context,0x44,1))return PL_BUS;
 
 *out=e;
 return PL_OK;
 
}
bool capture_data_owner_init(CaptureDataOwner*c,const PlRegs*r,DmaRx*a,DmaRx*b,const CaptureDataHooks*h,uint64_t limit){
 if(!c||!valid(r)||!a||!b||a==b||!h||!h->halt||!h->idle||!h->rearm||!limit)return false;
 
 memset(c,0,sizeof(*c));
 c->regs=*r;
 c->flows[0]=a;
 c->flows[1]=b;
 c->hooks=*h;
 c->limit_us=limit;
 return true;
 
}
static PlResult fail(CaptureDataOwner*c,PlResult p){c->failure=p;
 return p;
 }
PlResult capture_data_owner_step(CaptureDataOwner*c,uint64_t now){
 PlDataAction a;
 PlResult p;
 
 if(!c)return PL_INVALID;
 
 if(c->failure&&c->failure!=PL_TIMEOUT)return c->failure;
 
 p=pl_data_action(&c->regs,&a);
 
 if(c->ack_pending){
  uint32_t status=rd(&c->regs,0x2c);
 
  if(status!=2)return fail(c,PL_REJECTED);
 
  if(p==PL_EMPTY&&rd(&c->regs,0x18)==c->action.seq){c->active=false;
 c->ack_pending=false;
 return c->timed_out?PL_TIMEOUT:PL_OK;
 }
  if(p!=PL_OK||!same(a,c->action))return fail(c,PL_STALE);
 
 } else {
  if(p==PL_EMPTY)return c->active?fail(c,PL_STALE):PL_EMPTY;
 
  if(p!=PL_OK)return fail(c,p);
 
  if(!c->active){c->active=true;
 c->action=a;
 c->since=now;
 c->started=0;
 c->done=0;
 c->timed_out=false;
 c->failure=PL_OK;
 }
  if(!same(a,c->action))return fail(c,PL_STALE);
 
 }
 if(now<c->since||now-c->since>=c->limit_us){c->timed_out=true;
 c->failure=PL_TIMEOUT;
 }
 if(c->ack_pending)return c->timed_out?PL_TIMEOUT:PL_BUSY;
 
 for(unsigned i=0;i<2;i++)if(a.mask&(1u<<i)){
  unsigned bit=1u<<i;
 DmaRx*r=c->flows[i];
 
  if(!(c->started&bit)){
   if(a.reset){
    for(size_t j=0;j<r->count;j++)if(r->slots[j].state==DMA_CPU_OWNED)return c->timed_out?PL_TIMEOUT:PL_BUSY;
 
    dma_rx_pause_submissions(r,true);
 
    if(!dma_rx_reset_begin(r))return fail(c,PL_REJECTED);
 
   }else if(!c->hooks.halt(c->hooks.context,i))return fail(c,PL_REJECTED);
 
   c->started|=bit;
 
  }
  if(!(c->done&bit)){
   bool done=a.reset?dma_rx_reset_poll(r):c->hooks.idle(c->hooks.context,i);
 
   if(done){
    if(a.resume&&!c->hooks.rearm(c->hooks.context,i))return fail(c,PL_REJECTED);
 
    c->done|=bit;
 
   }
  }
 }
 if(c->done!=a.mask)return c->timed_out?PL_TIMEOUT:PL_BUSY;
 
 p=pl_data_ack(&c->regs,&a);
 
 if(p!=PL_OK)return fail(c,p);
 c->ack_pending=true;
 
 return c->timed_out?PL_TIMEOUT:PL_BUSY;
 
}
