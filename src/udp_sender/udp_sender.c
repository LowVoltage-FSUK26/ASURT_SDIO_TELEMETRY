#include "udp_sender.h"
#include "wifi_manager/wifi_manager.h"
#include "telemetry_packet.h"   /* Stage 6: stable wire-format */
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"            /* Stage 6: for esp_timer_get_time() */
#include "telemetry_config.h"
#include "driver/twai.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>

static const char *TAG = "udp_sender";
static int udp_sock = -1;
static struct sockaddr_in dest_addr;
static SemaphoreHandle_t udp_mutex = NULL;

#define WIFI_SEND_ERR 118
#define UDP_MAX_RETRIES    4
#define UDP_BASE_DELAY_MS 10

static uint32_t heap_log_counter = 0;
static uint32_t udp_seq = 0;  /* Stage 6: rolling sequence number for drop detection */

static bool init_udp_socket(void) {
    if (!udp_mutex) {
        udp_mutex = xSemaphoreCreateMutex();
        if (!udp_mutex) {
            ESP_LOGE(TAG, "Failed to create UDP mutex");
            return false;
        }
    }

    xSemaphoreTake(udp_mutex, portMAX_DELAY);

    if (udp_sock >= 0) {
        close(udp_sock);
    }

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) {
        int err = errno;
        xSemaphoreGive(udp_mutex);
        ESP_LOGE(TAG, "socket() failed: errno %d", err);
        return false;
    }

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family      = AF_INET;
    dest_addr.sin_port        = htons(SERVER_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    xSemaphoreGive(udp_mutex);
    return true;
}

void udp_socket_close(void) {
    if (!udp_mutex) {
        udp_mutex = xSemaphoreCreateMutex();
        if (!udp_mutex) {
            ESP_LOGE(TAG, "Failed to create UDP mutex");
            if (udp_sock >= 0) {
                close(udp_sock);
                udp_sock = -1;
            }
            return;
        }
    }

    xSemaphoreTake(udp_mutex, portMAX_DELAY);

    if (udp_sock >= 0) {
        close(udp_sock);
        udp_sock = -1;
        ESP_LOGI(TAG, "UDP socket closed due to Wi-Fi drop");
    }

    xSemaphoreGive(udp_mutex);
}

void udp_sender_task(void *pvParameters)
{
    QueueHandle_t telemetry_queue = (QueueHandle_t)pvParameters;
    EventGroupHandle_t eg = wifi_event_group();
    xEventGroupWaitBits(eg,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected; starting UDP sender");

    if (!init_udp_socket()) {
        ESP_LOGE(TAG, "Initial UDP socket setup failed");
        vTaskDelete(NULL);
        return;
    }

    twai_message_t current, newer;

    while (1) {
        if (xQueueReceive(telemetry_queue, &current, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Queue receive failed");
            continue;
        }

        if ((xEventGroupGetBits(eg) & WIFI_CONNECTED_BIT) == 0) {
            ESP_LOGW(TAG, "Wi-Fi lost, waiting to reconnect...");
            xEventGroupWaitBits(eg,
                                WIFI_CONNECTED_BIT,
                                pdFALSE,
                                pdTRUE,
                                portMAX_DELAY);
            ESP_LOGI(TAG, "Wi-Fi reconnected; re-initializing socket");
            init_udp_socket();
        }

        /* Stage 6 fix: drain queue FIRST so we always send the newest frame,
         * then run the retry loop with a properly advancing attempt counter.
         * Before this fix, `attempt = 0` inside the for-loop could spin forever
         * when the queue was fed faster than sendto() could complete. */
        while (xQueueReceive(telemetry_queue, &newer, 0) == pdTRUE) {
            current = newer;
        }

        /* Stage 6: convert twai_message_t → telemetry_packet_t (stable 24-byte wire format) */
        telemetry_packet_t pkt = {
            .seq          = udp_seq++,
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
            .can_id       = (uint16_t)current.identifier,
            .dlc          = current.data_length_code,
            .flags        = (current.extd ? 0x01 : 0x00) | (current.rtr ? 0x02 : 0x00),
        };
        memcpy(pkt.data, current.data, sizeof(pkt.data));

        bool sent = false;
        int last_err = 0;
        for (int attempt = 1; attempt <= UDP_MAX_RETRIES; ++attempt) {
            xSemaphoreTake(udp_mutex, portMAX_DELAY);
            int ret = sendto(udp_sock,
                             &pkt, sizeof(pkt), 0,  /* Stage 6: send telemetry_packet_t */
                             (struct sockaddr *)&dest_addr,
                             sizeof(dest_addr));
            last_err = errno;
            xSemaphoreGive(udp_mutex);
            if (ret < 0) {
                if (attempt == 1) {
                    ESP_LOGW(TAG,
                             "sendto failed (errno %d), retrying", last_err);
                    init_udp_socket();
                }
                if ((xEventGroupGetBits(eg) & WIFI_CONNECTED_BIT) == 0 ||
                    last_err == WIFI_SEND_ERR) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(UDP_BASE_DELAY_MS << (attempt - 1)));
            } else {
                sent = true;
                break;
            }
        }
        if (!sent) {
            if ((xEventGroupGetBits(eg) & WIFI_CONNECTED_BIT) == 0 ||
                last_err == WIFI_SEND_ERR) {
                ESP_LOGW(TAG, "Waiting for Wi-Fi to reconnect...");
                xEventGroupWaitBits(eg,
                                    WIFI_CONNECTED_BIT,
                                    pdFALSE,
                                    pdTRUE,
                                    portMAX_DELAY);
                ESP_LOGI(TAG, "Wi-Fi reconnected; re-initializing socket");
                init_udp_socket();
            } else {
                ESP_LOGE(TAG,
                         "Dropping packet after %d retries (errno %d)",
                         UDP_MAX_RETRIES,
                         last_err);
            }
        }

        // if (++heap_log_counter >= 1000) {
        //     size_t free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        //     ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)free);
        //     heap_log_counter = 0;
        // }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
