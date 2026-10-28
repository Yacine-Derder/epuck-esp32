#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "../main/bridge_transport.c"
static char diagnostic_log[32768];
void bridge_test_log(const char *level,const char *tag,const char *format,...) {
 (void)level;(void)tag; va_list args;va_start(args,format);
 size_t used=strlen(diagnostic_log); int n=vsnprintf(diagnostic_log+used,sizeof(diagnostic_log)-used,format,args);
 assert(n>=0 && used+(size_t)n+1U<sizeof(diagnostic_log)); strcat(diagnostic_log,"\n"); va_end(args);
}
static jmp_buf stop_loop;
static unsigned mock_connected, mock_fault, mock_writes;
static uint32_t mock_time;
static int mock_uart_read, mock_uart_write=1;
static const char *mock_input;
static unsigned mock_read_tick;
unsigned xEventGroupGetBits(void *group) { (void)group; return mock_connected; }
int xQueueReceive(void *queue,void *out,int ticks) {
 (void)queue;(void)ticks;
 if(!mock_fault) return 0; ((uart_event_t *)out)->type=(int)mock_fault; mock_fault=0; return 1;
}
int uart_read_bytes(int uart,void *buffer,unsigned length,int ticks) {
 (void)uart;assert(ticks==0);mock_time+=mock_read_tick;
 if(mock_input) { unsigned n=(unsigned)strlen(mock_input);if(n>length)n=length;memcpy(buffer,mock_input,n);mock_input+=n;if(!*mock_input)mock_input=NULL;return (int)n; }
 return mock_uart_read;
}
int uart_tx_chars(int uart,const char *bytes,unsigned length) { (void)uart;(void)bytes; assert(length); ++mock_writes; return mock_uart_write>0&&(unsigned)mock_uart_write>length?(int)length:mock_uart_write; /* partial FIFO write */ }
int uart_flush_input(int uart) { (void)uart; return 0; }
int gpio_set_level(int pin,int level) { (void)pin;(void)level; return 0; }
uint32_t esp_random(void) { static uint32_t value=1; return value++; }
int64_t esp_timer_get_time(void) { return (int64_t)mock_time*1000; }
int xTaskCreate(void (*task)(void *),const char *name,unsigned stack,void *arg,int priority,void *handle) {
 (void)task;(void)name;(void)stack;(void)arg;(void)priority;(void)handle; return pdPASS;
}
void vTaskDelay(unsigned ticks) { assert(ticks==1); longjmp(stop_loop,1); }
static int attach(const char *id) {
 int pair[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
 client=pair[0]; assert(fcntl(client,F_SETFL,O_NONBLOCK)==0);
 strcpy(session,id); ready=1; stm_diagnostic[0]=0; opened_at=last_progress=mock_time; memset(&tcp_out,0,sizeof(tcp_out)); memset(&uart_out,0,sizeof(uart_out));
 return pair[1];
}
static void owner_once(void) {
 if(!setjmp(stop_loop)) transport_task(NULL);
}
static void assert_reason(const char *reason) {
 char expected[96];snprintf(expected,sizeof(expected),"disconnect reason=%s ",reason);
 assert(strstr(diagnostic_log,expected)); assert(strstr(diagnostic_log,"uart_q=") && strstr(diagnostic_log,"age_ms="));
}
static void reason_tests(void) {
 int peer=attach("1111111111111111");
 ready=0; opened_at=0;mock_time=40;
 uart_record("SESSION 1111111111111111 READY");
 size_t logged=strlen(diagnostic_log); uart_record("SESSION 1111111111111111 READY");assert(strlen(diagnostic_log)==logged);
 assert(strstr(diagnostic_log,"handshake_ready session=1111111111111111 elapsed_ms=40"));
 memset(&tcp_out,0,sizeof(tcp_out));
 uart_record("DIAG 0000000000000000 reason=stale");assert(!stm_diagnostic[0]);
 uart_record("DIAG 1111111111111111 reason=control_hb_timeout hb_ms=500");
 uart_record("SESSION 1111111111111111 LOST"); assert_reason("stm32_session_lost");
 char notice[1024]={0};int n=(int)recv(peer,notice,sizeof(notice)-1U,0);assert(n>0);
 assert(strstr(notice,"DISCONNECT 1111111111111111 reason=stm32_session_lost") && strstr(notice,"control_hb_timeout"));
 logged=strlen(diagnostic_log);disconnect_client("duplicate",0);assert(strlen(diagnostic_log)==logged);close(peer);
 mock_connected=1; listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);assert(fcntl(listener,F_SETFL,O_NONBLOCK)==0);
 const unsigned faults[]={UART_FIFO_OVF,UART_BUFFER_FULL,UART_FRAME_ERR,UART_PARITY_ERR};
 const char *reasons[]={"uart_fifo_overflow","uart_buffer_overflow","uart_framing_error","uart_parity_error"};
 for(unsigned i=0;i<4;i++) { peer=attach("2222222222222222");mock_fault=faults[i];owner_once();assert(client==-1);assert_reason(reasons[i]);close(peer); }
 peer=attach("3333333333333333");mock_connected=0;bridge_transport_wifi_lost(201);owner_once();
 assert_reason("wifi_lost");assert(strstr(diagnostic_log,"detail=201"));close(peer);mock_connected=1;
 peer=attach("4444444444444444");mock_input="BAD\tLINE\n";owner_once();assert_reason("uart_malformed_record");close(peer);
 peer=attach("5555555555555555");memset(uart_parser.data,'X',BRIDGE_LINE-2U);uart_parser.length=BRIDGE_LINE-2U;mock_input="X";
 owner_once();assert_reason("uart_oversized_record");close(peer);
 peer=attach("6666666666666666");mock_uart_read=-1;owner_once();assert_reason("uart_read_error");close(peer);mock_uart_read=0;
 peer=attach("7777777777777777");assert(bridge_push(&uart_out,"OPEN 7777777777777777"));mock_uart_write=-1;
 owner_once();assert_reason("uart_write_error");close(peer);mock_uart_write=1;
 peer=attach("8888888888888888");close(peer);owner_once();assert_reason("tcp_receive_eof");
 peer=attach("9999999999999999");assert(send(peer,"M 1\t2\n",6,0)==6);owner_once();assert_reason("tcp_malformed_record");close(peer);
 peer=attach("aaaaaaaaaaaaaaaa");char oversized[65];memset(oversized,'X',sizeof(oversized));assert(send(peer,oversized,sizeof(oversized),0)==(int)sizeof(oversized));
 owner_once();assert_reason("tcp_oversized_record");close(peer);
 peer=attach("bbbbbbbbbbbbbbbb");ready=0;opened_at=0;mock_time=1000;owner_once();assert_reason("session_handshake_timeout");close(peer);
 peer=attach("cccccccccccccccc");int small=1024;assert(setsockopt(client,SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
 char fill[1024];memset(fill,'X',sizeof(fill));while(send(client,fill,sizeof(fill),send_flags())>0) {}
 assert(errno==EAGAIN || errno==EWOULDBLOCK);assert(bridge_push(&tcp_out,"LOG queued"));last_progress=0;mock_time=1500;
 owner_once();assert_reason("tcp_output_stalled");close(peer);
 peer=attach("dddddddddddddddd");assert(shutdown(peer,SHUT_RD)==0);assert(bridge_push(&tcp_out,"LOG queued"));
 owner_once();assert_reason("tcp_send_error");close(peer);
 peer=attach("eeeeeeeeeeeeeeee");close(client);owner_once();assert_reason("tcp_receive_error");close(peer);
 close(listener);listener=-1;
 /* Acceptance uses the real host listening socket, with the same sole owner. */
 listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
 struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(bind(listener,(struct sockaddr *)&address,sizeof(address))==0);assert(listen(listener,1)==0);
 socklen_t size=sizeof(address);assert(getsockname(listener,(struct sockaddr *)&address,&size)==0);
 peer=socket(AF_INET,SOCK_STREAM,0);assert(peer>=0);assert(connect(peer,(struct sockaddr *)&address,sizeof(address))==0);
 accept_client(mock_time);assert(client>=0 && strstr(diagnostic_log,"accepted session="));disconnect_client("test_cleanup",0);close(peer);close(listener);listener=-1;
 assert_reason("tcp_tx_queue_full");assert_reason("uart_tx_queue_full");
 peer=attach("abababababababab");assert(bridge_push(&tcp_out,"DATA partial"));
 assert(send(client,tcp_out.data[tcp_out.tail],3U,send_flags())==3);bridge_advance(&tcp_out,3U);
 disconnect_client("partial_record",0);
 char partial[64]={0};assert(recv(peer,partial,sizeof(partial),0)==3 && memcmp(partial,"DAT",3U)==0);
 assert(recv(peer,partial,sizeof(partial),0)==0);close(peer); /* Never splice DISCONNECT into the partial record. */
}
static void priority_tests(void) {
 int peer=attach("aabbccddaabbccdd");mock_connected=1;
 memset(&telemetry_out,0,sizeof(telemetry_out));
 mulTelemetry_t observation={.type=mulTlmTdoa,.node=141,.peer=133,.radio=2,.mode=5,.role=2,.epoch=99,.ratePolicy=1,.legacySlotMs=500,
 .tdoa={.reference=132,.follower=133,.status=4,.clockValid=1,.rateState=1,.ratePpb=0,.tdoaDtu=-332,.rangeDifferenceMm=-1558}};
 char line[MUL_TLM_LINE];
 /* Cross uint32 wrap while the optional queue is saturated every second. */
 mock_time=UINT32_MAX-5000U;
 for(unsigned cycle=0;cycle<600;cycle++) {
   mock_time+=1000;last_telemetry_stats=mock_time;
   mulTelemetry_t summary={.type=mulTlmSummary,.node=141,.peer=133,.radio=2,.mode=5,.role=2,.epoch=99};
   if(cycle%2) {summary.type=mulTlmClock;summary.clock=(mulClockTelemetry_t){.reference=132,.window=24,.fallback=1,.fitSigmaPpb=UINT32_MAX};}
   assert(mulTelemetryEncode(line,sizeof(line),&summary));
   for(unsigned i=0;i<BRIDGE_SLOTS;i++)uart_record(line);
   mulTelemetry_t r=observation;r.sequence=cycle;
   if(cycle%2){r.tdoa.status=1;r.tdoa.rateState=0;r.tdoa.ratePpb=INT32_MIN;r.tdoa.tdoaDtu=INT32_MIN;r.tdoa.rangeDifferenceMm=INT32_MIN;}
   if(cycle%3==2) {
     r.type=mulTlmVector;r.vector=(mulMultiCycle_t){.cycle=cycle,.count=3,.ids={132,133,134},.revision=1,.fingerprint=7,.slotMs=10,.periodMs=1000,.seenMask=3,.validMask=2,
       .values={{.rangeDifferenceMm=-1558,.tdoaDtu=-332,.status=4,.clockValid=1,.rateState=1},
                {.rangeDifferenceMm=INT32_MIN,.tdoaDtu=INT32_MIN,.status=0,.reason=1}}};
   }
   assert(mulTelemetryEncode(line,sizeof(line),&r));
   uint32_t drops=telemetry_drops;uart_record(line);
   assert(telemetry_out.count==BRIDGE_SLOTS&&telemetry_drops==drops+1);
   for(unsigned i=0;i<BRIDGE_SLOTS;i++) {
     owner_once();char output[512]={0};
     assert(recv(peer,output,sizeof(output),MSG_DONTWAIT)==310);
     if(i==BRIDGE_SLOTS-1)assert(!memcmp(output,line,309));
   }
 }
 for(unsigned i=0;i<BRIDGE_SLOTS;i++)uart_record(line);
 unsigned tail=telemetry_out.tail;uint32_t drops=telemetry_drops;
 queue_telemetry("UWBSTAT optional",0);assert(telemetry_drops==drops+1&&telemetry_out.tail==tail);
 uart_record(line);assert(telemetry_drops==drops+2);
 disconnect_client("test_cleanup",0);close(peer);
 /* Never evict a partial optional line or splice a disconnect notice into it. */
 peer=attach("abababababababab");queue_telemetry("UWBSTAT partial",0);
 assert(send(client,telemetry_out.data[telemetry_out.tail],3,send_flags())==3);bridge_advance(&telemetry_out,3);
 for(unsigned i=1;i<BRIDGE_SLOTS;i++)queue_telemetry("UWBSTAT optional",0);
 uart_record(line);assert(telemetry_out.offset==3&&!strncmp(telemetry_out.data[telemetry_out.tail],"UWBSTAT partial",14));
 disconnect_client("partial_telemetry",0);char partial[512];assert(recv(peer,partial,sizeof(partial),0)==3);assert(recv(peer,partial,sizeof(partial),0)==0);close(peer);
 puts("600s priority/invalid observations/uint32 wrap/full queue/partial line protection passed");
}
static void uwb_telemetry_tests(void) {
 int peer=attach("aabbccddaabbccdd");mulTelemetry_t r={.type=mulTlmRange,.node=0x10,.peer=0x11,.radio=2,.mode=1,.role=1,.epoch=1,.rawMm=1234};char line[MUL_TLM_LINE];
 assert(mulTelemetryEncode(line,sizeof(line),&r));
 for(unsigned i=0;i<BRIDGE_SLOTS+3;i++)uart_record(line);
 assert(client>=0&&telemetry_out.count==BRIDGE_SLOTS&&telemetry_drops>=3&&tcp_out.count==0);
 assert(bridge_push(&tcp_out,"[MOTOR] OK STOP"));
 /* Full optional queue never disconnects or consumes reserved control slots. */
 char bad[MUL_TLM_LINE];memcpy(bad,line,sizeof(bad));bad[30]='?';uint32_t corrupt=telemetry_corrupt;uart_record(bad);assert(telemetry_corrupt==corrupt+1&&client>=0);
 bridge_parser_t parser={0};for(unsigned i=0;i<strlen(line);i++)assert(bridge_parse(&parser,(unsigned char)line[i],318)==0);
 assert(bridge_parse(&parser,'\n',318)==1&&mulTelemetryDecode(parser.data,&r));
 memset(&telemetry_out,0,sizeof(telemetry_out));memset(&tcp_out,0,sizeof(tcp_out));
 r=(mulTelemetry_t){.type=mulTlmTdoa,.node=141,.peer=133,.radio=2,.mode=5,.role=2,.epoch=99,.ratePolicy=1,.legacySlotMs=500,
 .tdoa={.reference=132,.follower=133,.referenceSequence=3666,.followerSequence=3676,.status=4,.clockValid=1,.rateState=1,.ratePpb=-12,.tdoaDtu=-28,.rangeDifferenceMm=-131}};
 assert(mulTelemetryEncode(line,sizeof(line),&r));mock_connected=1;mock_time=100;last_telemetry_stats=100;
 uart_record(line);assert(telemetry_out.count==1);owner_once();
 char forwarded[512]={0};ssize_t bytes=recv(peer,forwarded,sizeof(forwarded)-1,0);assert(bytes==310&&!memcmp(forwarded,line,309)&&forwarded[309]=='\n');
 forwarded[309]=0;mulTelemetry_t decoded;assert(mulTelemetryDecode(forwarded,&decoded)&&decoded.tdoa.rangeDifferenceMm==-131&&decoded.tdoa.followerSequence==3676);
 /* Tick advancement inside UART RX must not expire a fresh record. */
 for(unsigned i=0;i<200;i++) {
   char wire[MUL_TLM_LINE+1];snprintf(wire,sizeof(wire),"%s\n",line);
   mock_input=wire;owner_once();
   mock_read_tick=1;owner_once();mock_read_tick=0;
   assert(telemetry_out.count==0);
   memset(forwarded,0,sizeof(forwarded));
   bytes=recv(peer,forwarded,sizeof(forwarded)-1,MSG_DONTWAIT);
   assert(bytes==310&&!memcmp(forwarded,line,309));
 }
 for(unsigned i=0;i<BRIDGE_SLOTS+2;i++)uart_record(line);assert(telemetry_out.count==BRIDGE_SLOTS);
 disconnect_client("test_cleanup",0);close(peer);assert(!telemetry_out.count);
 uart_record(line);assert(!telemetry_out.count);
 puts("UWB optional queue saturation/CRC/frame recovery/control isolation/disconnected purge passed");
}
static void ekf_transport_tests(void) {
 while(app_ekf_worker_step()){}
 uint32_t generation;char output[EKF_SERVICE_LINE];
 while(app_ekf_receive(&generation,output)){}
 app_ekf_reset(++ekf_generation);
 int peer=attach("123456789abcdef0");
 tcp_record("EKF STATUS");assert(!uart_out.count);
 assert(app_ekf_worker_step()&&app_ekf_receive(&generation,output));
 assert(generation==ekf_generation&&strstr(output,"enabled=0")&&strstr(output,"DATA2_measured=0"));
 assert(app_ekf_submit(ekf_generation,"EKF WORKER",0));assert(app_ekf_worker_step());
 assert(app_ekf_receive(&generation,output)&&strstr(output,"stack_min_free_bytes=UNKNOWN"));
 for(unsigned i=0;i<8;i++)assert(app_ekf_submit(ekf_generation,"EKF STATUS",0));
 assert(!app_ekf_submit(ekf_generation,"EKF STATUS",0));
 uint32_t dropped=app_ekf_dropped();assert(dropped);
 // A saturated optional input queue never prevents reserved control forwarding.
 tcp_record("CONTROL_HB");assert(uart_out.count==1&&strstr(uart_out.data[uart_out.tail],"CONTROL_HB"));
 while(app_ekf_worker_step()){}
 assert(app_ekf_submit(ekf_generation,"EKF STATUS",0));assert(app_ekf_worker_step());
 assert(app_ekf_dropped()>dropped); // saturated output is bounded, too
 while(app_ekf_receive(&generation,output)){}
 // Stale queued configuration/ON commands are discarded on connection reset.
 assert(app_ekf_submit(ekf_generation,"EKF CONFIG EXPERIMENTAL .2 .0001 1e-9 1e-8 0 16 10000 100000 400000 0 1000 3 1",0));
 disconnect_client("test_ekf_reset",0);close(peer);
 while(app_ekf_worker_step()){}
 assert(!app_ekf_receive(&generation,output));
 assert(app_ekf_submit(ekf_generation,"EKF STATUS",0));assert(app_ekf_worker_step());
 assert(app_ekf_receive(&generation,output)&&strstr(output,"config=0"));
 // Long EKF-only input through actual TCP parser; normal control keeps63B limit.
 peer=attach("223456789abcdef0");mock_connected=wifi_bit=1;mock_uart_write=128;
 listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);assert(fcntl(listener,F_SETFL,O_NONBLOCK)==0);
 char pose[171]="EKF POSE ";memset(pose+9,'0',160);pose[169]='\n';pose[170]=0;
 assert(send(peer,pose,170,0)==170);owner_once();assert(client>=0&&ready);
 assert(!uart_out.count); // locally rejected pose never reaches STM32
 char input[1024];assert(recv(peer,input,sizeof(input),MSG_DONTWAIT)>0);
 disconnect_client("test_cleanup",0);close(peer);close(listener);listener=-1;
 while(app_ekf_worker_step()){}
 puts("EKF default OFF, bounded worker saturation, control isolation, session purge and long local TCP input passed");
}
static void replay_diagnostics(void) {
 char line[310];uint8_t b[152]={0x4d,0x54,3,15,141,255,2,5,2,0,1,1};
 b[16]=1;b[41]=4;b[42]=128;b[43]=1;b[48]=100;b[49]=1;
 const char *hex="0123456789ABCDEF";
 for(unsigned test=0;test<4;test++) {
  b[11]=test==1?99:1;b[41]=test==2?3:4;
  uint16_t crc=mulTelemetryCrc(b,150);b[150]=crc;b[151]=crc>>8;
  memcpy(line,"UWB3 ",5);for(unsigned i=0;i<152;i++){line[5+i*2]=hex[b[i]>>4];line[6+i*2]=hex[b[i]&15];}line[309]=0;
  if(test==3)line[40]='?';
  assert(tdoa_replay_wire(line)==(test>=2?-1:1));
 }
 puts("Replay extension: unchanged frame limit, schema forwarding, malformed parts and CRC validation passed");
}
int main(void) {
 replay_diagnostics();
 /* Generic streaming commands and both telemetry versions pass through the
  * actual transport, preserving envelopes and per-session replies. */
 int stream_peer=attach("0123456789abcdef");
 const char *stream_commands[]={"STREAM 100","STREAM 50","STREAM 0","STREAM_STATUS"};
 for(unsigned i=0;i<4;i++) {
  tcp_record(stream_commands[i]); size_t n; const char *record=bridge_front(&uart_out,&n);
  char expected[96]; snprintf(expected,sizeof(expected),"CMD 0123456789abcdef %s\n",stream_commands[i]);
  assert(record && strcmp(record,expected)==0); bridge_advance(&uart_out,n);
 }
 uart_record("RSP 0123456789abcdef [STREAM] OK STREAM acknowledged=50 ms");
 assert(strncmp(tcp_out.data[tcp_out.tail],"[STREAM] OK STREAM",18)==0);
 memset(&tcp_out,0,sizeof(tcp_out));
 for(unsigned i=0;i<20;i++) {
  char data[256]; snprintf(data,sizeof(data),"DATA %u 0 7 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 -20 21",i);
  if (i%2) snprintf(data,sizeof(data),"DATA2 %u 0 7 0 0 0 0 0 0 0 0 1 2 3 -20 21",i);
  uart_record(data); size_t n; const char *record=bridge_front(&tcp_out,&n);
  assert(record && strncmp(record,data,strlen(data))==0); bridge_advance(&tcp_out,n);
  assert(client>=0 && ready);
 }
 disconnect_client("test_cleanup",0); close(stream_peer);
 int peer=attach("aaaaaaaaaaaaaaaa");
 tcp_record("M 200 200"); tcp_record("CONTROL_HB");
 assert(uart_out.count==2 && strncmp(uart_out.data[uart_out.tail],"CMD aaaaaaaaaaaaaaaa M 200 200",29)==0);
 bridge_advance(&uart_out,3); disconnect_client("test_cleanup",0);
 assert(client==-1 && uart_out.count==1 && uart_out.offset==0 && tcp_out.count==0);
 assert(strncmp(uart_out.data[uart_out.tail],"\nCLOSE aaaaaaaaaaaaaaaa",23)==0); close(peer);
 peer=attach("bbbbbbbbbbbbbbbb");
 uart_record("RSP aaaaaaaaaaaaaaaa [MOTOR] OK ARM"); assert(!tcp_out.count);
 uart_record("RSP bbbbbbbbbbbbbbbb [MOTOR] OK ARM"); assert(tcp_out.count==1);
 assert(strncmp(tcp_out.data[tcp_out.tail],"[MOTOR] OK ARM",13)==0);
 memset(&tcp_out,0,sizeof(tcp_out));
 /* Slow TCP client: reception remains bounded, output overflow invalidates. */
 for(unsigned i=0;i<=BRIDGE_SLOTS;i++) uart_record("DATA 1 0 0");
 assert(client==-1 && tcp_out.count==0 && uart_out.count==1); close(peer);
 peer=attach("cccccccccccccccc"); ready=0;
 tcp_record("ARM"); assert(uart_out.count==0 && tcp_out.count==1); disconnect_client("test_cleanup",0); close(peer);
 peer=attach("dddddddddddddddd");
 for(unsigned i=0;i<=BRIDGE_SLOTS;i++) tcp_record("M 1 1");
 assert(client==-1 && uart_out.count==1 && strncmp(uart_out.data[uart_out.tail],"\nCLOSE ",7)==0); close(peer);
 /* The actual owner loop responds to Wi-Fi loss and does a partial write. */
 peer=attach("eeeeeeeeeeeeeeee"); wifi_bit=1; mock_connected=0;
 if(!setjmp(stop_loop)) transport_task(NULL);
 assert(client==-1 && mock_writes==1 && uart_out.offset==1); close(peer);
 /* UART fault likewise invalidates from the owner, not an ISR/socket race. */
 peer=attach("ffffffffffffffff"); mock_connected=1; mock_fault=1;
 listener=socket(AF_INET,SOCK_STREAM,0); assert(listener>=0); assert(fcntl(listener,F_SETFL,O_NONBLOCK)==0);
 if(!setjmp(stop_loop)) transport_task(NULL);
 assert(client==-1 && uart_out.count==1); close(peer); close(listener); listener=-1;
 reason_tests();uwb_telemetry_tests();priority_tests();ekf_transport_tests();
 puts("PASS: disconnect diagnostics, nonblocking notification, real transport session filtering, control/output overflow, slow-client loss, Wi-Fi/UART faults and nonblocking partial writes");
}
