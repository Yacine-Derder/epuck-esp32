#include "mul_antenna_offset.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 assert(mulAntennaOffsetRound(5,2)==3&&mulAntennaOffsetRound(-5,2)==-3);
 assert(mulAntennaOffsetRound(4,2)==2&&mulAntennaOffsetRound(-4,2)==-2);
 for(unsigned platform=0;platform<2;platform++){
  uint16_t bt=platform?16380:14498,br=platform?16380:18453,tx,rx;
  assert(mulAntennaOffsetApply(0,bt,br,&tx,&rx)&&tx==bt&&rx==br);
  assert(mulAntennaOffsetApply(50,bt,br,&tx,&rx)&&tx==bt+9&&rx==br+12);
  assert((tx-bt)+(rx-br)==21);
  assert(mulAntennaOffsetApply(-50,bt,br,&tx,&rx)&&tx==bt-9&&rx==br-12);
  assert((tx-bt)+(rx-br)==-21);
  int32_t um;assert(mulAntennaOffsetEquivalentUm(bt,br,tx,rx,&um)&&um==-49264);
  assert(!mulAntennaOffsetEquivalentUm(bt,br,tx+1,rx,&um));
  assert(mulAntennaOffsetParse("+50",bt,br,&tx,&rx)&&tx==bt+9);
  const char *bad[]={"","+","-","1.5","1x","2147483648","-2147483649","999999999999","2147483647","-2147483648"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){tx=123;rx=456;assert(!mulAntennaOffsetParse(bad[i],bt,br,&tx,&rx));assert(tx==123&&rx==456);}
  // Quantization is many-to-one; never pretend the requested scalar is stored.
  uint16_t a,b;assert(mulAntennaOffsetApply(49,bt,br,&tx,&rx));
  assert(mulAntennaOffsetApply(50,bt,br,&a,&b)&&a==tx&&b==rx);
 }
 puts("Antenna offsets: symmetric ties, absolute baselines, signs/split, bounds, strict parsing and quantized equivalents passed");
}
