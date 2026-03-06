/*
 * rtc_time_sync.c
 *
 *  Created on  :   Jun 22, 2025
 *      Author  :   Mina Fathy
 *  Description :   This module initializes SNTP to retrieve the current time from NTP servers
 *                  and sets the system time on the ESP32. It uses the internal RTC timer to
 *                  maintain time between synchronizations. Includes optional utility to retrieve
 *                  formatted time strings for logging or display.
 */

#include "rtc_time_sync.h"
#include "esp_timer.h"       // Stage 7: needed for esp_timer_get_time() (ms resolution)
#include "telemetry_config.h" // Stage 7: TIMEZONE_STR

static const char *TAG = "rtc_time";



void Time_Sync_init_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void Time_Sync_obtain_time(void)
{
    Time_Sync_init_sntp();

    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for SNTP sync...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    setenv("TZ", TIMEZONE_STR, 1);  // Stage 7: use configurable timezone from telemetry_config.h
    tzset();
}

uint8_t Time_Sync_get_rtc_time_str(char *buffer, uint8_t max_len)
{
    time_t now = time(NULL);
    struct tm timeinfo;
    if (!localtime_r(&now, &timeinfo)) return false;

    /* Stage 7: append millisecond resolution (.mmm) via esp_timer */
    int64_t time_us = esp_timer_get_time();
    int ms = (time_us / 1000) % 1000;

    char base[24];
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &timeinfo);
    snprintf(buffer, max_len, "%s.%03d", base, ms);
    return true;
} 

