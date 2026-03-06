#include "connectivity.h"
#include "wifi_manager/wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "telemetry_config.h"

static const char *TAG = "conn_check";

static bool connectivity_test(void)
{
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONNECTIVITY_TEST_PORT);
    dest.sin_addr.s_addr = inet_addr(CONNECTIVITY_TEST_IP);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed");
        return false;
    }

    /* Stage 7: apply send/receive timeout so the probe doesn't block forever */
    struct timeval tv = {
        .tv_sec  = CONNECTIVITY_TCP_TIMEOUT_S,
        .tv_usec = 0
    };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int err = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    close(sock);
    return (err == 0);
}

void connectivity_monitor_task(void *pvParameters)
{
    (void)pvParameters;
    EventGroupHandle_t eg = wifi_event_group();
    int fails = 0;

    ESP_LOGI("connectivity_monitor_task", "Running on core %d", xPortGetCoreID());


    while (1) {
        xEventGroupWaitBits(eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(CONNECTIVITY_CHECK_INTERVAL_MS));

        if (connectivity_test()) {
            if (fails != 0) {
                ESP_LOGI(TAG, "Connectivity restored after %d failed attempt%s", fails, (fails == 1 ? "" : "s"));
            }
            fails = 0;
        } else {
            ++fails;
            int remaining = CONNECTIVITY_FAIL_THRESHOLD - fails;
            ESP_LOGW(TAG,
                     "Connectivity test failed (%d/%d). %d try%s left before forcing Wi-Fi reconnect",
                     fails,
                     CONNECTIVITY_FAIL_THRESHOLD,
                     remaining,
                     (remaining == 1 ? "" : "s"));
            if (fails >= CONNECTIVITY_FAIL_THRESHOLD) {
                fails = 0;
                ESP_LOGW(TAG,
                         "No connectivity after %d attempts — forcing Wi-Fi reconnect now",
                         CONNECTIVITY_FAIL_THRESHOLD);
                wifi_force_reconnect();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}
