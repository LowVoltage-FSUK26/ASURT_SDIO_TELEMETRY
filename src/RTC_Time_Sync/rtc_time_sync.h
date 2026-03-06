/*
 * rtc_time_sync.h
 *
 *  Created on: Jun 22, 2025
 *      Author: Mina Fathy
 */

#ifndef RTC_TIME_SYNC_H
#define RTC_TIME_SYNC_H

//--------------------------------
//includes
//--------------------------------
#include <time.h>
#include "ff.h"     // For DWORD
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

//===============================================
// APIs Supported by "RTC_Time_Sync DRIVER"
//===============================================
void Time_Sync_init_sntp(void);
void Time_Sync_obtain_time(void);
uint8_t Time_Sync_get_rtc_time_str(char *buffer, uint8_t max_len);
/* Stage 7: removed dead wifi_connect() declaration — not implemented anywhere */
#endif // RTC_TIME_SYNC_H