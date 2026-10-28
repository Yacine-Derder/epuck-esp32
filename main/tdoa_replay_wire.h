/* Diagnostic extension to fixed-size UWB3; canonical measurement codec untouched. */
#ifndef TDOA_REPLAY_WIRE_H
#define TDOA_REPLAY_WIRE_H
#include "mul_telemetry.h"
#include <string.h>
static int replay_hex(char c) {
 if(c>='0'&&c<='9')return c-'0';
 if(c>='A'&&c<='F')return c-'A'+10;
 if(c>='a'&&c<='f')return c-'a'+10;
 return -1;
}
/* 0 = other type, 1 = valid diagnostic, -1 = corrupt diagnostic.
 * Unknown diagnostic schema is forwarded intact for explicit host reporting. */
static int tdoa_replay_wire(const char *line) {
 if(strncmp(line,"UWB3 ",5)||strlen(line)!=309)return 0;
 if(replay_hex(line[11])!=0||replay_hex(line[12])!=15)return 0;
 uint8_t b[152];
 for(unsigned i=0;i<152;i++){
  int hi=replay_hex(line[5+2*i]),lo=replay_hex(line[6+2*i]);
  if(hi<0||lo<0)return -1;
  b[i]=(uint8_t)((hi<<4)|lo);
 }
 if(b[0]!='M'||b[1]!='T'||b[2]!=3||b[6]!=2||b[8]!=2||b[7]<4||b[7]>5||b[9]||b[10]!=1||
    mulTelemetryCrc(b,150)!=(uint16_t)(b[150]|(uint16_t)b[151]<<8))return -1;
 if(b[11]!=1)return 1;
 if(b[40]>=4||b[41]!=4||b[42]!=128||b[43]!=1||b[48]!=(b[40]==3?84:100)||b[49]==0||b[49]>=8)return -1;
 for(unsigned i=50+b[48];i<150;i++)if(b[i])return -1;
 return 1;
}
#endif
