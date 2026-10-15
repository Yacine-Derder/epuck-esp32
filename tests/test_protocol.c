#include "bridge_protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
 bridge_parser_t p={0}; bridge_queue_t q={0};
 const char *text="ARM\r\nM -200 200\nCONTROL_HB\n"; unsigned lines=0;
 for(;*text;++text) if(bridge_parse(&p,(unsigned char)*text,63)>0) ++lines;
 assert(lines==3);
 assert(bridge_parse(&p,'\r',63)==0); assert(bridge_parse(&p,'x',63)==-1);
 assert(bridge_parse(&p,'\n',63)==-1); assert(bridge_parse(&p,'\n',63)==0);
 for(unsigned i=0;i<64;i++) bridge_parse(&p,'x',63);
 assert(bridge_parse(&p,'\n',63)==-1);
 for(unsigned i=0;i<BRIDGE_SLOTS;i++) assert(bridge_push(&q,"CMD aaaaaaaaaaaaaaaa M 200 200"));
 assert(!bridge_push(&q,"overflow")); size_t n; const char *front=bridge_front(&q,&n);
 assert(n>3); char before[320]; strcpy(before,q.data[q.tail]);
 bridge_advance(&q,3); assert(q.count==BRIDGE_SLOTS && q.offset==3);
 assert(memcmp(bridge_front(&q,&n),before+3,n)==0);
 bridge_advance(&q,n); assert(q.count==BRIDGE_SLOTS-1 && q.offset==0);
 assert(bridge_reply_matches("RSP aaaaaaaaaaaaaaaa [MOTOR] OK M","aaaaaaaaaaaaaaaa"));
 assert(!bridge_reply_matches("RSP aaaaaaaaaaaaaaaa [MOTOR] OK M","bbbbbbbbbbbbbbbb"));
 assert(!bridge_reply_matches("RSP a","aaaaaaaaaaaaaaaa"));
 /* Session invalidation resets both partial output and queued movement. */
 memset(&q,0,sizeof(q)); assert(bridge_push(&q,"\nCLOSE aaaaaaaaaaaaaaaa"));
 assert(bridge_push(&q,"OPEN bbbbbbbbbbbbbbbb")); front=bridge_front(&q,&n);
 assert(strncmp(front,"\nCLOSE ",7)==0); bridge_advance(&q,n); front=bridge_front(&q,&n); assert(strncmp(front,"OPEN ",5)==0);
 puts("PASS: fragmented/coalesced lines, malformed/oversized recovery, bounded queues, partial output, stale-session replies and replacement ordering");
}
