#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#define WIFI_SSID       "disal-robots"
#define WIFI_PASS       "dis@l1234"

#define TCP_PORT        3333

#define UART_PORT       UART_NUM_1
#define UART_TX_PIN     43
#define UART_RX_PIN     44
#define UART_BUF_SIZE   1024

#define LED_GPIO        GPIO_NUM_46

static const char *TAG = "UART_TCP_BRIDGE";

static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static volatile int client_sock = -1;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

static void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
                                 UART_TX_PIN,
                                 UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART ready on TX=%d RX=%d", UART_TX_PIN, UART_RX_PIN);
}

static void led_init(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(LED_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 0));
    ESP_LOGI(TAG, "LED ready on GPIO %d", LED_GPIO);
}

static void tcp_server_task(void *pvParameters)
{
    int listen_sock;
    struct sockaddr_in server_addr;

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(TCP_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", TCP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        ESP_LOGI(TAG, "Waiting for TCP client...");
        int sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "accept() failed: errno %d", errno);
            continue;
        }

        ESP_LOGI(TAG, "Client connected: %s", inet_ntoa(client_addr.sin_addr));

        if (client_sock >= 0) {
            close(client_sock);
        }
        client_sock = sock;

        // Optional welcome message
        const char *msg = "ESP32 UART->TCP bridge connected\r\n";
        send(client_sock, msg, strlen(msg), 0);

        // Keep this task alive until client disconnects
        char dummy[32];
        while (1) {
            int len = recv(client_sock, dummy, sizeof(dummy), MSG_DONTWAIT);
            if (len == 0) {
                ESP_LOGI(TAG, "Client disconnected");
                close(client_sock);
                client_sock = -1;
                break;
            } else if (len < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                } else {
                    ESP_LOGW(TAG, "Client socket error, closing: errno %d", errno);
                    close(client_sock);
                    client_sock = -1;
                    break;
                }
            }
        }
    }
}

static void uart_reader_task(void *pvParameters)
{
    uint8_t data[UART_BUF_SIZE + 1];

    while (1) {
        int len = uart_read_bytes(UART_PORT, data, UART_BUF_SIZE, pdMS_TO_TICKS(50));
        if (len > 0) {
            data[len] = 0;

            ESP_LOGI(TAG, "UART RX (%d bytes): %s", len, (char *)data);

            // 🔵 Blink LED on activity
            gpio_set_level(LED_GPIO, 1);  // ON

            // Keep your logic
            if (strstr((char *)data, "STM32 LED off") != NULL) {
                ESP_LOGI(TAG, "ESP LED ON (command)");
            } else if (strstr((char *)data, "STM32 LED on") != NULL) {
                ESP_LOGI(TAG, "ESP LED OFF (command)");
            }

            // Send over TCP
            if (client_sock >= 0) {
                int sent = send(client_sock, data, len, 0);
                if (sent < 0) {
                    ESP_LOGW(TAG, "send() failed, closing client: errno %d", errno);
                    close(client_sock);
                    client_sock = -1;
                }
            }

            // 🔵 Short blink duration
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(LED_GPIO, 0);  // OFF
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_init();
    uart_init();
    wifi_init_sta();

    xTaskCreate(tcp_server_task, "tcp_server_task", 4096, NULL, 5, NULL);
    xTaskCreate(uart_reader_task, "uart_reader_task", 4096, NULL, 5, NULL);
}