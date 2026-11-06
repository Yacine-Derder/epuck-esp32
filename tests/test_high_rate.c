#define send test_send
#define main legacy_main
#include "test_transport.c"
#undef main
#undef send
extern ssize_t send(int,const void *,size_t,int);
static unsigned send_limit,send_calls;
ssize_t test_send(int fd,const void *bytes,size_t size,int flags) {
 if(send_limit){if(++send_calls%5U==0){errno=EAGAIN;return -1;}if(size>send_limit)size=send_limit;}
 return send(fd,bytes,size,flags);
}
static char wire[8192];static unsigned wire_used;
static uint64_t generated,received_bytes;static unsigned kinds[16],samples,acks;
static unsigned sent_times[64],max_ack_latency;
static char received_line[320];static unsigned received_used;
static void append(const char *line) {
 unsigned n=(unsigned)strlen(line);assert(wire_used+n+1<sizeof(wire));memcpy(wire+wire_used,line,n);wire_used+=n;wire[wire_used++]='\n';generated+=n+1;
}
static void receive_all(int peer) {
 char data[4096];int n;
 while((n=(int)recv(peer,data,sizeof(data),MSG_DONTWAIT))>0)for(int i=0;i<n;i++) {
  received_bytes++;
  if(data[i]!='\n'){assert(received_used<sizeof(received_line)-1);received_line[received_used++]=data[i];continue;}
  received_line[received_used]=0;received_used=0;
  if(!strncmp(received_line,"UWB3 ",5)) {mulTelemetry_t r;assert(mulTelemetryDecode(received_line,&r));kinds[r.type]++;}
  else if(!strncmp(received_line,"DATA2 ",6)) {
   unsigned sequence,age,source;int left,right;
   assert(sscanf(received_line,"DATA2 %u %u %u %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %d %d",&sequence,&age,&source,&left,&right)==5);
   assert(sequence==(uint32_t)(UINT32_MAX-1500U+samples) && source==(uint16_t)(samples*100U));
   assert(age==10 && left==INT32_MIN && right==INT32_MAX);samples++;
  } else if(strstr(received_line,"[MOTOR] OK")) {
   unsigned latency=mock_time-sent_times[acks++];if(latency>max_ack_latency)max_ack_latency=latency;
  }
 }
 assert(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK));
}
int main(void) {
 int peer=attach("0123456789abcdef");mock_connected=wifi_bit=1;mock_uart_write=128;
 listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);assert(fcntl(listener,F_SETFL,O_NONBLOCK)==0);
 mulTelemetry_t vector={.type=mulTlmPlanVector,.quality=4,.node=141,.radio=2,.mode=5,.role=2,.epoch=1,.ratePolicy=1,
  .vector={.count=4,.ids={132,134,133,135},.revision=1,.fingerprint=0xEC2B4B2A,.slotMs=5,.periodMs=100,.seenMask=15,.validMask=14}};
 for(unsigned i=0;i<3;i++)vector.vector.values[i]=(mulMultiValue_t){.status=4,.clockValid=1,.rateState=1};
 mulTelemetry_t raw=vector;raw.type=mulTlmPlanRx;raw.legacyLength=90;raw.legacySlotMs=10;raw.scheduleRevision=1;raw.scheduleFingerprint=0xEC2B4B2A;raw.legacyFrame[84]=4;raw.legacyFrame[86]=0x2a;raw.legacyFrame[87]=0x4b;raw.legacyFrame[88]=0x2b;raw.legacyFrame[89]=0xec;
 mulTelemetry_t clock={.type=mulTlmClock,.node=141,.radio=2,.role=2,.mode=5,.epoch=1,
  .clock={.reference=132,.window=24,.fallback=1,.fitSigmaPpb=UINT32_MAX}};
 mulTelemetry_t ar={.type=mulTlmPlanRange,.quality=4,.radio=2,.remoteRadio=1,.mode=4,.role=2,.node=141,.peer=132,.flags=9,.epoch=1,
  .planRange={.fingerprint=0xEC2B4B2A,.revision=1,.initiator=132,.responder=141,.replyMs=5,.rawTof=100,.tof=100,.unknownComponents=255,.source=1,.version=1}};
 for(unsigned i=0;i<3;i++)ar.planRange.power[i]=(mulPower_t){.powerCentiDbm=INT16_MIN,.status=mulPowerUnsupported};
 char encoded[4][MUL_TLM_LINE];mulTelemetry_t *records[]={&vector,&ar,&clock,&raw};
 for(unsigned i=0;i<4;i++) {if(!mulTelemetryEncode(encoded[i],sizeof(encoded[i]),records[i])) {fprintf(stderr,"encode fixture %u failed\n",i);assert(0);}}
 char driver[2049]={0};unsigned driver_used=0,source_samples=0,wire_credit=0,commands=0;
 for(unsigned ms=0;ms<300000;ms++) {
  if(ms%100==0){append(encoded[0]);char data[160];snprintf(data,sizeof(data),"DATA2 %u 10 %u -32768 -32768 -32768 -32768 -32768 -32768 -32768 -32768 -32768 -32768 -32768 -2147483648 2147483647",UINT32_MAX-1500U+source_samples,(uint16_t)(source_samples*100U));append(data);source_samples++;}
  if(ms%250==0)append(encoded[1]);if(ms%200==0)append(encoded[2]);
  if(ms*18U/1000U!=(ms+1U)*18U/1000U)append(encoded[3]);
  if(ms%5000==0) {
   const char *cmd=ms%10000?"M -50 60\n":"M 50 -60\n";
   assert(send(peer,cmd,strlen(cmd),0)==(ssize_t)strlen(cmd));sent_times[commands++]=ms;
  }
  wire_credit+=BRIDGE_UART_BAUD;unsigned n=wire_credit/10000U;wire_credit%=10000U;if(n>wire_used)n=wire_used;
  assert(driver_used+n<sizeof(driver));memcpy(driver+driver_used,wire,n);driver_used+=n;driver[driver_used]=0;
  memmove(wire,wire+n,wire_used-n);wire_used-=n;
  if(ms%10==9) {
   mock_time=ms;mock_input=driver;owner_once();assert(!mock_input);driver_used=0;driver[0]=0;
   /* The real UART command queue drained one complete framed M. Simulate the
    * unchanged STM32 ACK response; no external ARM/HB was requested. */
   if(ms%5000==9){append("RSP 0123456789abcdef [MOTOR] OK M left=50 right=-60");assert(uart_out.count==0);}
   receive_all(peer);assert(client>=0 && ready && !sensor_drops && !telemetry_drops && !telemetry_corrupt && !tcp_control_full);
  }
 }
 /* Flush the final serial burst, then issue STOP while queues are active. */
 sent_times[commands]=300000;assert(send(peer,"STOP\n",5,0)==5);
 for(unsigned ms=300000;ms<300200;ms++) {
  unsigned n=wire_used>92?92:wire_used;assert(driver_used+n<sizeof(driver));memcpy(driver+driver_used,wire,n);driver_used+=n;driver[driver_used]=0;memmove(wire,wire+n,wire_used-n);wire_used-=n;
  if(ms%10==9){mock_time=ms;mock_input=driver;owner_once();driver_used=0;if(ms==300009)append("RSP 0123456789abcdef [MOTOR] OK STOP");receive_all(peer);}
 }
 assert(samples==3000 && kinds[mulTlmPlanVector]==3000 && kinds[mulTlmPlanRange]==1200 && kinds[mulTlmClock]==1500 && kinds[mulTlmPlanRx]==5400);
 assert(mock_writes==commands+1 && max_ack_latency<=50 && acks==commands+1 && !sensor_drops && !telemetry_drops && !tcp_control_full && !uart_out.count);
 printf("300s ESP simulation: TDoA=%u AR=%u clock=%u rawRX=%u DATA2=%u ACKs=%u UART=%lluB TCP=%lluB (%.1fB/s); no drops/disconnect; max command ACK latency=%ums\n",kinds[mulTlmPlanVector],kinds[mulTlmPlanRange],kinds[mulTlmClock],kinds[mulTlmPlanRx],samples,acks,(unsigned long long)generated,(unsigned long long)received_bytes,(double)received_bytes/300.,max_ack_latency);
 /* Force short sends/EAGAIN across all three output classes. */
 tcp_uwb_streak=0;send_limit=7;acks=0;assert(bridge_push(&tcp_out,"[MOTOR] OK STOP"));uart_record(encoded[0]);uart_record("DATA2 0 10 0 0 0 0 0 0 0 0 0 0 0 0 -2147483648 2147483647");
 char joined[1024]={0};size_t joined_size=0;
 for(unsigned i=0;i<100;i++){
  drain_tcp(mock_time+i);ssize_t n;
  while((n=recv(peer,joined+joined_size,sizeof(joined)-joined_size-1,MSG_DONTWAIT))>0)joined_size+=(size_t)n;
 }
 char expected[1024];snprintf(expected,sizeof(expected),"[MOTOR] OK STOP\n%s\nDATA2 0 10 0 0 0 0 0 0 0 0 0 0 0 0 -2147483648 2147483647\n",encoded[0]);
 assert(!strcmp(joined,expected));
 assert(!tcp_out.count && !sensor_out.count && !telemetry_out.count && client>=0);
 disconnect_client("test_cleanup",0);close(peer);close(listener);puts("partial writes/EAGAIN bounded recovery passed");
}
