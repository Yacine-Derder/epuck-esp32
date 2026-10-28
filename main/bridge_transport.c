#include "app_ekf.h"
#include "bridge_transport.h"
#include "bridge_protocol.h"
#include "mul_telemetry.h"
#include "tdoa_replay_wire.h"
#ifndef BRIDGE_HOST_TEST
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>

/* One task exclusively owns BOTH sockets, parsers and queues. Wi-Fi event
 * callbacks only change a FreeRTOS event group; no cross-task socket closes.
 */
static EventGroupHandle_t wifi_group;
static EventBits_t wifi_bit;
static QueueHandle_t uart_events;
static int uart_port, led_gpio, listener=-1, client=-1;
static bridge_parser_t tcp_parser, uart_parser;
static bridge_queue_t uart_out, tcp_out, telemetry_out;
static uint32_t telemetry_at[BRIDGE_SLOTS],telemetry_drops,telemetry_corrupt,telemetry_uart_errors,telemetry_high,last_telemetry_stats;
static unsigned char telemetry_essential[BRIDGE_SLOTS];
static char session[17];
static uint32_t ekf_generation;
static unsigned ready;
static uint32_t opened_at, last_progress, led_until;
static const char *const BRIDGE_TAG="bridge";
static char stm_diagnostic[224];
static atomic_int wifi_loss_detail;
void bridge_transport_wifi_lost(int reason) { atomic_store(&wifi_loss_detail,reason); }
static int send_flags(void)
{
  int flags=MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
  flags|=MSG_NOSIGNAL;
#endif
  return flags;
}
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }

static void disconnect_client(const char *reason, int detail)
{
  char loss[64];
  if (client<0) return; /* Exactly one diagnostic per active-session teardown. */
  app_ekf_reset(++ekf_generation);
  uint32_t now=now_ms();
  ESP_LOGW(BRIDGE_TAG,
      "disconnect reason=%s session=%s detail=%d uart_q=%u tcp_q=%u age_ms=%" PRIu32 " stall_ms=%" PRIu32 " stm_diag=%s",
      reason,session,detail,uart_out.count,tcp_out.count,
      (uint32_t)(now-opened_at),(uint32_t)(now-last_progress),
      stm_diagnostic[0]?stm_diagnostic:"-");
  /* One nonblocking attempt only. Do not splice a notice into a partially
   * transmitted record, drain queues, wait for ACKs or defer STOP/CLOSE.
   */
  if (tcp_out.offset==0U && telemetry_out.offset==0U) {
    char notice[384];
    int n=snprintf(notice,sizeof(notice),"DISCONNECT %s reason=%s detail=%d stm=%s\n",
                   session,reason,detail,stm_diagnostic[0]?stm_diagnostic:"-");
    if (n>0 && n<(int)sizeof(notice)) (void)send(client,notice,(size_t)n,send_flags());
  }
  close(client); client=-1; ready=0;
  telemetry_drops+=telemetry_out.count;memset(&telemetry_out,0,sizeof(telemetry_out));
  memset(&tcp_out,0,sizeof(tcp_out)); memset(&tcp_parser,0,sizeof(tcp_parser));
  /* Cancel unsent old movement and delimit any UART line partly written.
   * CLOSE precedes any replacement OPEN; stale RSPs are session filtered.
   */
  memset(&uart_out,0,sizeof(uart_out));
  snprintf(loss,sizeof(loss),"\nCLOSE %s",session);
  (void)bridge_push(&uart_out,loss);
  memset(&uart_parser,0,sizeof(uart_parser));
}
static void listener_error(const char *reason,int code)
{
  static const char *last_reason;
  static int last_code;
  static uint32_t logged_at, suppressed;
  uint32_t now=now_ms();
  if (!last_reason || strcmp(last_reason,reason)!=0 || last_code!=code || (uint32_t)(now-logged_at)>=1000U) {
    ESP_LOGE(BRIDGE_TAG,"listener reason=%s session=- detail=%d port=3333 suppressed=%" PRIu32,reason,code,suppressed);
    last_reason=reason; last_code=code; logged_at=now; suppressed=0U;
  } else ++suppressed;
}
static int create_listener(void)
{
  int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_IP), one=1;
  struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(3333),.sin_addr.s_addr=htonl(INADDR_ANY)};
  if (fd<0) { listener_error("tcp_socket_error",errno); return -1; }
  setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
  if (bind(fd,(struct sockaddr *)&address,sizeof(address))<0) {
    listener_error("tcp_bind_error",errno); close(fd); return -1;
  }
  if (listen(fd,1)<0) { listener_error("tcp_listen_error",errno); close(fd); return -1; }
  if (fcntl(fd,F_SETFL,O_NONBLOCK)<0) { listener_error("tcp_listener_nonblocking_error",errno); close(fd); return -1; }
  return fd;
}
static void accept_client(uint32_t now)
{
  int fd=accept(listener,NULL,NULL);
  if (fd<0) return;
  if (client>=0) {
    ESP_LOGW(BRIDGE_TAG,"reject reason=controller_busy session=%s socket=%d",session,fd);
    close(fd); return;
  }
  if (fcntl(fd,F_SETFL,O_NONBLOCK)<0) {
    ESP_LOGW(BRIDGE_TAG,"reject reason=socket_nonblocking_error session=- detail=%d socket=%d",errno,fd);
    close(fd); return;
  }
  client=fd; ready=0; opened_at=last_progress=now;
  app_ekf_reset(++ekf_generation);
  snprintf(session,sizeof(session),"%08" PRIx32 "%08" PRIx32,esp_random(),esp_random());
  telemetry_drops+=telemetry_out.count;memset(&telemetry_out,0,sizeof(telemetry_out));
  memset(&tcp_out,0,sizeof(tcp_out)); memset(&tcp_parser,0,sizeof(tcp_parser));
  stm_diagnostic[0]=0;
  ESP_LOGI(BRIDGE_TAG,"accepted session=%s socket=%d uart_q=%u tcp_q=%u",session,client,uart_out.count,tcp_out.count);
  char record[64]; snprintf(record,sizeof(record),"OPEN %s",session);
  if (!bridge_push(&uart_out,record)) disconnect_client("uart_tx_queue_full",0);
}
/* Bounded FIFO with observation admission priority. Never replace a partly
 * transmitted line; keep original sequence order among retained records. */
static void queue_telemetry(const char *line,unsigned essential) {
  bridge_queue_t *q=&telemetry_out;
  if(q->count==BRIDGE_SLOTS) {
    unsigned victim=0;
    if(essential) {
      while(victim<q->count && (telemetry_essential[(q->tail+victim)%BRIDGE_SLOTS] ||
            (victim==0 && q->offset)))victim++;
    }
    if(!essential || victim==q->count){telemetry_drops++;return;}
    for(unsigned i=victim;i+1<q->count;i++) {
      unsigned dst=(q->tail+i)%BRIDGE_SLOTS,src=(q->tail+i+1)%BRIDGE_SLOTS;
      memcpy(q->data[dst],q->data[src],BRIDGE_LINE);q->length[dst]=q->length[src];
      telemetry_at[dst]=telemetry_at[src];telemetry_essential[dst]=telemetry_essential[src];
    }
    q->head=(q->head+BRIDGE_SLOTS-1)%BRIDGE_SLOTS;q->count--;telemetry_drops++;
  }
  telemetry_at[q->head]=now_ms();telemetry_essential[q->head]=(unsigned char)essential;
  if(!bridge_push(q,line))telemetry_drops++;
  if(q->count>telemetry_high)telemetry_high=q->count;
}
static void uart_record(const char *line)
{
  char expected[64];
  if(strncmp(line,"UWB1 ",5)==0||strncmp(line,"UWB2 ",5)==0||strncmp(line,"UWB3 ",5)==0) {
    int replay=tdoa_replay_wire(line);
    if(replay){
      if(replay<0){telemetry_corrupt++;return;}
      if(client<0||!ready){telemetry_drops++;return;}
      queue_telemetry(line,0);return;
    }
    mulTelemetry_t record;
    if(!mulTelemetryDecode(line,&record)){telemetry_corrupt++;return;}
    if(client<0||!ready){telemetry_drops++;return;}
    queue_telemetry(line,record.type==mulTlmPlanRange||record.type==mulTlmPlanVector||record.type==mulTlmVector||record.type==mulTlmTdoa||record.type==mulTlmRange||record.type==mulTlmObservation);
    return;
  }
  if (client<0) return;
  snprintf(expected,sizeof(expected),"SESSION %s READY",session);
  if (strcmp(line,expected)==0 && !ready) {
    ready=1U;
    ESP_LOGI(BRIDGE_TAG,"handshake_ready session=%s elapsed_ms=%" PRIu32 " uart_q=%u tcp_q=%u",
             session,(uint32_t)(now_ms()-opened_at),uart_out.count,tcp_out.count);
  }
  snprintf(expected,sizeof(expected),"SESSION %s LOST",session);
  if (strcmp(line,expected)==0) { disconnect_client("stm32_session_lost",0); return; }
  if (strncmp(line,"DIAG ",5U)==0) {
    if (strlen(line)<22U || strncmp(line+5,session,16U)!=0 || line[21]!=' ') return;
    snprintf(stm_diagnostic,sizeof(stm_diagnostic),"%.223s",line+22);
  } else if (strncmp(line,"SESSION ",8U)==0) {
    snprintf(expected,sizeof(expected),"SESSION %s READY",session);
    if (strcmp(line,expected)!=0) return;
  } else if (strncmp(line,"RSP ",4U)==0) {
    if (!bridge_reply_matches(line,session)) return;
    line+=21;
  } else if (strncmp(line,"DATA ",5U)!=0 && strncmp(line,"DATA2 ",6U)!=0 && strncmp(line,"LOG ",4U)!=0) return;
  if (!bridge_push(&tcp_out,line)) disconnect_client("tcp_tx_queue_full",0); /* Slow client cannot block RX. */
}
static void tcp_record(const char *line)
{
  char record[128];
  if (!ready) {
    if (!bridge_push(&tcp_out,"ERR STM32 session not ready; retry after SESSION READY")) disconnect_client("tcp_tx_queue_full",0);
    return;
  }
  if (!strcmp(line,"EKF") || !strncmp(line,"EKF ",4)) {
    if(!app_ekf_submit(ekf_generation,line,now_ms()))queue_telemetry("EKF ERR worker unavailable/queue full; command not accepted",0);
    return;
  }
  snprintf(record,sizeof(record),"CMD %s %s",session,line);
  if (!bridge_push(&uart_out,record)) disconnect_client("uart_tx_queue_full",0);
}
static void transport_task(void *argument)
{
  (void)argument;
  for (;;) {
    uint32_t now=now_ms();
    unsigned connected=(xEventGroupGetBits(wifi_group)&wifi_bit)!=0;
    if (!connected) disconnect_client("wifi_lost",atomic_load(&wifi_loss_detail));
    if (connected && listener<0) listener=create_listener();
    if (connected && listener>=0) accept_client(now);
    uart_event_t event;
    for (unsigned budget=0; budget<4 && xQueueReceive(uart_events,&event,0); ++budget) {
      if (event.type==UART_FIFO_OVF || event.type==UART_BUFFER_FULL || event.type==UART_FRAME_ERR || event.type==UART_PARITY_ERR) {
        const char *reason=event.type==UART_FIFO_OVF?"uart_fifo_overflow":
                           event.type==UART_BUFFER_FULL?"uart_buffer_overflow":
                           event.type==UART_FRAME_ERR?"uart_framing_error":"uart_parity_error";
        telemetry_uart_errors++;disconnect_client(reason,event.type); uart_flush_input(uart_port); memset(&uart_parser,0,sizeof(uart_parser));
      }
    }
    uint8_t bytes[256];
    int n=uart_read_bytes(uart_port,bytes,sizeof(bytes),0);
    if (n<0) disconnect_client("uart_read_error",n);
    if (n>0) {
      gpio_set_level(led_gpio,1); led_until=now+50U;
      for (int i=0;i<n;++i) {
        int parsed=bridge_parse(&uart_parser,bytes[i],BRIDGE_LINE-2U);
        if (parsed<0) {
          disconnect_client(uart_parser.length>=BRIDGE_LINE-2U?"uart_oversized_record":"uart_malformed_record",(int)uart_parser.length);
          memset(&uart_parser,0,sizeof(uart_parser));
        }
        else if (parsed>0) uart_record(uart_parser.data);
      }
    }
    if ((int32_t)(now-led_until)>=0) gpio_set_level(led_gpio,0);
    if (client>=0) {
      n=recv(client,bytes,sizeof(bytes),MSG_DONTWAIT);
      if (n==0) disconnect_client("tcp_receive_eof",0);
      else if (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) disconnect_client("tcp_receive_error",errno);
      else if (n>0) {
        for (int i=0;i<n && client>=0;++i) {
          size_t limit=tcp_parser.length>=4 && !memcmp(tcp_parser.data,"EKF ",4)?EKF_SERVICE_LINE-2U:63U;
          int parsed=bridge_parse(&tcp_parser,bytes[i],limit);
          if (parsed<0) disconnect_client(tcp_parser.length>=limit?"tcp_oversized_record":"tcp_malformed_record",(int)tcp_parser.length);
          else if (parsed>0) tcp_record(tcp_parser.data);
        }
      }
    }
    /* Optional results cannot consume the reserved control reply queue. */
#ifdef BRIDGE_HOST_TEST
    app_ekf_worker_step();
#endif
    char ekf_line[EKF_SERVICE_LINE];uint32_t generation;
    for(unsigned budget=0;budget<2 && app_ekf_receive(&generation,ekf_line);budget++)
      if(client>=0 && ready && generation==ekf_generation)queue_telemetry(ekf_line,0);
    size_t remaining;
    const char *front=bridge_front(&uart_out,&remaining);
    if (front) {
      n=uart_tx_chars(uart_port,front,remaining); /* TX driver buffering is disabled: nonblocking FIFO write. */
      if (n>0) bridge_advance(&uart_out,(size_t)n);
      else if (n<0) disconnect_client("uart_write_error",n);
    }
    /* uart_record timestamps at receipt, which can be later than the loop's
     * initial snapshot. Refresh before unsigned age arithmetic: otherwise a
     * 1ms tick crossing makes a fresh record appear ~49 days old. */
    now=now_ms();
    if(telemetry_out.count&&(uint32_t)(now-telemetry_at[telemetry_out.tail])>=1000U) {
      if(telemetry_out.offset)disconnect_client("telemetry_partial_stalled",0);
      else {telemetry_drops++;bridge_advance(&telemetry_out,telemetry_out.length[telemetry_out.tail]);}
    }
    if(client>=0&&ready&&(uint32_t)(now-last_telemetry_stats)>=1000U&&tcp_out.count<BRIDGE_SLOTS-4U) {
      char stats[224];last_telemetry_stats=now;
      snprintf(stats,sizeof(stats),"UWBSTAT version=1 dropped=%" PRIu32 " corrupt=%" PRIu32 " uart_errors=%" PRIu32 " depth=%u high=%" PRIu32 " ekf_dropped=%" PRIu32,
        telemetry_drops,telemetry_corrupt,telemetry_uart_errors,telemetry_out.count,telemetry_high,app_ekf_dropped());
      queue_telemetry(stats,0);
    }
    /* A partially sent line must finish before switching queue. Otherwise all
     * safety/control replies take precedence over optional telemetry. */
    bridge_queue_t *chosen=telemetry_out.offset?&telemetry_out:tcp_out.count?&tcp_out:&telemetry_out;
    if (client>=0 && (front=bridge_front(chosen,&remaining))!=NULL) {
      n=send(client,front,remaining,send_flags());
      if (n>0) { bridge_advance(chosen,(size_t)n); last_progress=now; }
      else if (n==0) disconnect_client("tcp_send_zero",0);
      else if (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) disconnect_client("tcp_send_error",errno);
      else if ((uint32_t)(now-last_progress)>=500U) disconnect_client("tcp_output_stalled",errno);
    } else if (client>=0) last_progress=now;
    if (client>=0 && !ready && (uint32_t)(now-opened_at)>=1000U) disconnect_client("session_handshake_timeout",0);
    vTaskDelay(1); /* No delay inside UART RX or socket writes. */
  }
}
void bridge_transport_start(EventGroupHandle_t wifi,EventBits_t connected,int uart,int led,QueueHandle_t events)
{
  wifi_group=wifi; wifi_bit=connected; uart_port=uart; led_gpio=led; uart_events=events;
  configASSERT(xTaskCreate(transport_task,"bridge_transport",6144,NULL,5,NULL)==pdPASS);
}
