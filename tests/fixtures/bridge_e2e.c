/* Real bridge parser/owner/TCP socket with simulated UART and clock ticks. */
#define _POSIX_C_SOURCE 200809L
#define main bridge_regressions_main
#include "../test_transport.c"
#undef main
#include <time.h>
int main(int argc,char **argv) {
 assert(argc==2);FILE *input=fopen(argv[1],"r");assert(input);
 char records[128][MUL_TLM_LINE+1];unsigned recordCount=0;
 while(recordCount<128&&fgets(records[recordCount],sizeof(records[0]),input))recordCount++;
 assert(recordCount>0&&feof(input));
 fclose(input);
 listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
 struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(bind(listener,(struct sockaddr *)&address,sizeof(address))==0&&listen(listener,1)==0);
 assert(fcntl(listener,F_SETFL,O_NONBLOCK)==0);
 socklen_t size=sizeof(address);assert(getsockname(listener,(struct sockaddr *)&address,&size)==0);
 printf("%u\n",ntohs(address.sin_port));fflush(stdout);
 mock_connected=wifi_bit=1;mock_read_tick=1;mock_uart_write=128;
 unsigned sent=0,completed=0;
 for(unsigned step=0;step<2000&&completed<2;step++) {
   mock_time+=10;owner_once();
   if(client>=0&&!ready) {
     char response[64];snprintf(response,sizeof(response),"SESSION %s READY",session);
     uart_record(response);sent=0;
   }
   if(client>=0&&ready&&step%10==0) {
     assert(sent<recordCount);mock_input=records[sent++];
     owner_once();owner_once(); /* fragmented UART and clock advancement */
     assert(!mock_input);
     for(unsigned i=0;i<BRIDGE_SLOTS+1&&(telemetry_out.count||tcp_out.count);i++)owner_once();
     assert(!telemetry_out.count&&!tcp_out.count);
     if(sent==recordCount){disconnect_client("test_reconnect",0);completed++;}
   }
   const struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,NULL);
 }
 if(client>=0)disconnect_client("test_cleanup",0);
 close(listener);return completed==2?0:1;
}
