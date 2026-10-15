#ifndef BRIDGE_TEST_PLATFORM_H
#define BRIDGE_TEST_PLATFORM_H
#include <stdint.h>
#include <assert.h>
typedef void *EventGroupHandle_t;
typedef unsigned EventBits_t;
typedef void *QueueHandle_t;
typedef struct { int type; } uart_event_t;
enum { UART_FIFO_OVF=1, UART_BUFFER_FULL, UART_FRAME_ERR, UART_PARITY_ERR };
void bridge_test_log(const char *level, const char *tag, const char *format, ...);
#define ESP_LOGE(tag, ...) bridge_test_log("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) bridge_test_log("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) bridge_test_log("I", tag, __VA_ARGS__)
#define pdPASS 1
#define configASSERT(x) assert(x)
unsigned xEventGroupGetBits(void *group);
int xQueueReceive(void *queue,void *event,int ticks);
int uart_read_bytes(int uart,void *buffer,unsigned length,int ticks);
int uart_tx_chars(int uart,const char *bytes,unsigned length);
int uart_flush_input(int uart);
int gpio_set_level(int pin,int level);
uint32_t esp_random(void);
int64_t esp_timer_get_time(void);
int xTaskCreate(void (*task)(void *),const char *name,unsigned stack,void *argument,int priority,void *handle);
void vTaskDelay(unsigned ticks);
#endif
