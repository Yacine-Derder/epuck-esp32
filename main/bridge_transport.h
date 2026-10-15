#ifndef BRIDGE_TRANSPORT_H
#define BRIDGE_TRANSPORT_H
#ifdef BRIDGE_HOST_TEST
#include "bridge_test_platform.h"
#else
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#endif
/* Event callback only records a code; transport task still owns sockets. */
void bridge_transport_wifi_lost(int reason);
void bridge_transport_start(EventGroupHandle_t wifi, EventBits_t connected,
                            int uart, int led, QueueHandle_t uart_events);
#endif
