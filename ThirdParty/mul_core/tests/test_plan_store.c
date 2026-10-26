#include "mul_plan_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t memory[512],saved[512];
static int bytesLeft=-1;
static unsigned reads,writes,failRead;
static bool readBytes(void *ctx,unsigned at,void *out,size_t n) {
 (void)ctx;assert(at>=128&&at+n<=352);reads++;if(reads==failRead)return false;
 memcpy(out,memory+at,n);return true;
}
static bool writeBytes(void *ctx,unsigned at,const void *data,size_t n) {
 (void)ctx;assert(at>=128&&at+n<=352);writes++;
 for(size_t i=0;i<n;i++){if(!bytesLeft)return false;if(bytesLeft>0)bytesLeft--;memory[at+i]=((const uint8_t *)data)[i];}
 return true;
}
static mulPlanStoreWorkspace_t workspace;
static const mulPlanStore_t store={NULL,readBytes,writeBytes,&workspace};
static mulRangingPlan_t example(void) {
 mulRangingPlan_t p={.replyMs=5,.maxUtilization=70,.enabled=3,.rlrCount=2,.rlrs={141,142},.aaMilliHz=500,.arMilliHz=2000};
 mulScheduleLegacy(&p.broadcast,132,133,10);mulScheduleExperimental(&p.broadcast);
 p.broadcast.count=4;p.broadcast.ids[2]=134;p.broadcast.ids[3]=135;p.broadcast.slotMs=3;
 p.broadcast.txLeadUs=1000;p.broadcast.rxBudgetUs=500;p.broadcast.rangingFlags=0;
 assert(!mulRangingPlanValidate(&p,NULL));return p;
}
static void untouchedPrefix(void){for(unsigned i=0;i<100;i++)assert(memory[i]==(uint8_t)i);}
static void crc(uint8_t *record){uint32_t c=mulScheduleCrc(record,76);for(unsigned i=0;i<4;i++)record[76+i]=(uint8_t)(c>>(8*i));}
int main(void) {
 mulRangingPlan_t p=example();uint32_t generation=99;mulStoredPlan_t loaded={.generation=42};
 memset(memory,255,sizeof(memory));for(unsigned i=0;i<100;i++)memory[i]=(uint8_t)i;
 assert(mulPlanStoreLoad(&store,&loaded)==mulPlanAbsent&&loaded.generation==42);
 memcpy(saved,memory,sizeof(memory));
 for(int cut=0;cut<=100;cut++) {
  memcpy(memory,saved,sizeof(memory));bytesLeft=cut;reads=writes=0;bool ok=mulPlanStoreSave(&store,&p,true,&generation);bytesLeft=-1;
  mulPlanStoreResult_t status=mulPlanStoreLoad(&store,&loaded);assert(status==(ok?mulPlanStored:mulPlanAbsent));
  if(ok)assert(loaded.selected&&loaded.generation==1);
  untouchedPrefix();
 }
 memcpy(memory,saved,sizeof(memory));assert(mulPlanStoreSave(&store,&p,true,&generation));
 memcpy(saved,memory,sizeof(memory));
 // Interrupt every byte during both alternate-slot creation and slot reuse.
 for(unsigned round=0;round<3;round++) {
  for(int cut=0;cut<=100;cut++) {
   memcpy(memory,saved,sizeof(memory));bytesLeft=cut;reads=writes=0;
   bool ok=mulPlanStoreSave(&store,&p,round%2!=0,&generation);bytesLeft=-1;
   assert(mulPlanStoreLoad(&store,&loaded)==mulPlanStored);
   assert(loaded.selected==(ok?(round%2!=0):(round%2==0)));
   assert(loaded.generation==(ok?round+2:round+1));untouchedPrefix();
  }
  memcpy(saved,memory,sizeof(memory));
 }
 writes=0;assert(mulPlanStoreSave(&store,&p,false,&generation)&&!writes); // Idempotent.
 // A lost readback ACK after commit must not be interpreted as no write.
 reads=0;failRead=3;assert(!mulPlanStoreSave(&store,&p,true,&generation));failRead=0;
 assert(mulPlanStoreLoad(&store,&loaded)==mulPlanStored&&loaded.selected&&loaded.generation==5);
 // Explicit generation32 rollover remains serially ordered.
 unsigned at=MUL_PLAN_STORE_BASE+loaded.slot*MUL_PLAN_STORE_STRIDE;
 memset(memory+at+8,255,4);crc(memory+at);
 // Remove the older committed slot so its artificial generation is unambiguous.
 memset(memory+MUL_PLAN_STORE_BASE+(loaded.slot^1u)*MUL_PLAN_STORE_STRIDE,255,MUL_PLAN_STORE_BYTES);
 assert(mulPlanStoreSave(&store,&p,false,&generation)&&generation==0);
 assert(mulPlanStoreLoad(&store,&loaded)==mulPlanStored&&loaded.generation==0&&!loaded.selected);
 // No valid committed copy: CRC damage is corruption, not blank defaults.
 memset(memory+MUL_PLAN_STORE_BASE+(loaded.slot^1u)*MUL_PLAN_STORE_STRIDE,255,MUL_PLAN_STORE_BYTES);
 memory[MUL_PLAN_STORE_BASE+loaded.slot*MUL_PLAN_STORE_STRIDE+12]^=1;
 assert(mulPlanStoreLoad(&store,&loaded)==mulPlanCorrupt);
 assert(!mulPlanStoreSave(&store,&p,true,&generation));
 reads=0;failRead=1;assert(mulPlanStoreLoad(&store,&loaded)==mulPlanIoError);failRead=0;
 untouchedPrefix();
 puts("Plan EEPROM journal: both slots, all100-byte cuts/reuse, selected state, idempotence, lost commit ACK, generation32 rollover, CRC/IO failure and legacy0..99 preservation passed");
 return 0;
}
