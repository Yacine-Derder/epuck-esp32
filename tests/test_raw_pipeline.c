#include <stdlib.h>
/* Replay timestamped STM32 UART bytes through the actual ESP transport owner.
 * Local UNIX socketpair only. No robot endpoint, motor/config command or DDS. */
#define main existing_transport_tests
#include "test_transport.c"
#undef main
static FILE *tcp;
static unsigned received[16],input_records;
static char line[BRIDGE_LINE];static unsigned used;
static void receive_all(int peer) {
 char bytes[4096];int n;
 while((n=(int)recv(peer,bytes,sizeof bytes,MSG_DONTWAIT))>0)for(int i=0;i<n;i++) {
  if(bytes[i]!='\n'){assert(used+1<sizeof line);line[used++]=bytes[i];continue;}
  line[used]=0;used=0;
  if(tcp)fprintf(tcp,"%s\n",line);
  if(!strncmp(line,"UWB3 ",5)) {
   unsigned type;assert(sscanf(line+11,"%2x",&type)==1&&type<16);received[type]++;
  }
 }
 assert(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK));
}
int main(int argc,char **argv) {
 assert(argc==3||argc==4);FILE *uart=fopen(argv[1],"r");assert(uart);tcp=fopen(argv[2],"w");assert(tcp);
 bool pressure=argc==4;
 int peer=attach("0123456789abcdef");mock_connected=wifi_bit=1;mock_uart_write=128;
 if(pressure){int small=1024;assert(!setsockopt(client,SOL_SOCKET,SO_SNDBUF,&small,sizeof small));}
 listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);assert(fcntl(listener,F_SETFL,O_NONBLOCK)==0);
 char record[512];unsigned at=0;
 while(fgets(record,sizeof record,uart)) {
  char *tab=strchr(record,'\t');assert(tab);*tab++=0;unsigned time=(unsigned)strtoul(record,NULL,10);
  assert(time>=at);at=time;mock_time=time;mock_input=tab;input_records++;owner_once();assert(!mock_input&&client>=0&&ready);
  if(!pressure||(time%1000)>=400)receive_all(peer);
 }
 for(unsigned i=0;i<100;i++){mock_time++;mock_input=NULL;owner_once();receive_all(peer);}
 assert(!used&&!telemetry_corrupt);
 if(!pressure)assert(!telemetry_drops);
 fprintf(stdout,"{\"esp_input_records\":%u,\"esp_raw_delivered\":%u,\"esp_vectors_delivered\":%u,\"esp_twr_delivered\":%u,\"esp_drops\":%u,\"esp_corrupt\":%u,\"pressure\":%u}\n",
 input_records,received[12],received[11],received[13],telemetry_drops,telemetry_corrupt,pressure);
 fclose(uart);fclose(tcp);close(peer);close(client);close(listener);
}
