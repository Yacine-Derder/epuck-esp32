/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_plan_store.h"
#include <string.h>
#define MAGIC UINT32_C(0x314a504d)
#define COMMIT UINT32_C(0x54494d43)
static uint32_t get(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static bool decode(const uint8_t *b,mulStoredPlan_t *out) {
 if(get(b)!=MAGIC||b[4]!=1||b[5]>1||b[6]||b[7]||get(b+92)!=COMMIT||get(b+76)!=mulScheduleCrc(b,76))return false;
 for(unsigned i=80;i<92;i++)if(b[i])return false;
 mulStoredPlan_t p={.generation=get(b+8),.selected=b[5]};
 if(!mulRangingPlanDecode(b+12,&p.plan))return false;
 *out=p;return true;
}
mulPlanStoreResult_t mulPlanStoreLoad(const mulPlanStore_t *s,mulStoredPlan_t *out) {
 if(!s||!s->read||!s->workspace||!out)return mulPlanIoError;
 uint8_t *b=s->workspace->bytes;mulStoredPlan_t best={0};bool valid=false,committed=false;
 for(unsigned slot=0;slot<2;slot++) {
  if(!s->read(s->context,MUL_PLAN_STORE_BASE+slot*MUL_PLAN_STORE_STRIDE,b,MUL_PLAN_STORE_BYTES))return mulPlanIoError;
  if(get(b+92)==COMMIT)committed=true;
  mulStoredPlan_t p;if(!decode(b,&p))continue;p.slot=(uint8_t)slot;
  if(valid) {
   uint32_t advance=p.generation-best.generation;
   if(advance==UINT32_C(0x80000000))return mulPlanCorrupt;
   if(!advance) {
    uint8_t *a=s->workspace->verify,*c=s->workspace->bytes;
    if(!mulRangingPlanEncode(a,&p.plan)||!mulRangingPlanEncode(c,&best.plan)||p.selected!=best.selected||memcmp(a,c,MUL_PLAN_BYTES))return mulPlanCorrupt;
   }else if(advance<UINT32_C(0x80000000))best=p;
  }else {best=p;valid=true;}
 }
 if(!valid)return committed?mulPlanCorrupt:mulPlanAbsent;
 *out=best;return mulPlanStored;
}
bool mulPlanStoreSave(const mulPlanStore_t *s,const mulRangingPlan_t *p,bool selected,uint32_t *generation) {
 if(!s||!s->write||!s->workspace||!generation||mulRangingPlanValidate(p,NULL))return false;
 mulStoredPlan_t old;mulPlanStoreResult_t status=mulPlanStoreLoad(s,&old);
 if(status==mulPlanIoError||status==mulPlanCorrupt)return false;
 uint8_t *b=s->workspace->bytes;memset(b,0,MUL_PLAN_STORE_BYTES);if(!mulRangingPlanEncode(b+12,p))return false;
 if(status==mulPlanStored&&old.selected==(unsigned)selected) {
  uint8_t *prior=s->workspace->verify;if(!mulRangingPlanEncode(prior,&old.plan))return false;
  if(!memcmp(prior,b+12,MUL_PLAN_BYTES)){*generation=old.generation;return true;}
 }
 uint32_t gen=status==mulPlanStored?old.generation+1:1;
 unsigned slot=status==mulPlanStored?old.slot^1u:0;
 unsigned at=MUL_PLAN_STORE_BASE+slot*MUL_PLAN_STORE_STRIDE;
 put(b,MAGIC);b[4]=1;b[5]=selected;put(b+8,gen);put(b+76,mulScheduleCrc(b,76));put(b+92,COMMIT);
 const uint8_t invalid[4]={0};
 // Invalidate only the inactive slot, then payload, then commit last. A false
 // return after the final write may still mean committed: always reload on boot.
 if(!s->write(s->context,at+92,invalid,sizeof(invalid))||!s->write(s->context,at,b,92)||!s->write(s->context,at+92,b+92,4))return false;
 uint8_t *verify=s->workspace->verify;mulStoredPlan_t decoded;
 if(!s->read(s->context,at,verify,MUL_PLAN_STORE_BYTES)||memcmp(b,verify,MUL_PLAN_STORE_BYTES)||!decode(verify,&decoded))return false;
 *generation=gen;return true;
}
