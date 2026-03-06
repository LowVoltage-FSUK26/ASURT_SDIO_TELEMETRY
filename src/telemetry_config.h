#ifndef TELEMETRY_CONFIG_H
#define TELEMETRY_CONFIG_H

#include "credentials.h"

// ── Master Feature Flags ────────────────────────────────────────
// Subsystem enable/disable (1 = enabled, 0 = disabled).
// Set any flag to 0 to compile out that subsystem entirely,
// allowing bench testing without all hardware connected.
#define USE_WIFI     1   // Wi-Fi init, telemetry tasks, connectivity monitor
#define USE_SDIO     1   // SD card init and logging task
#define USE_CAN      1   // TWAI driver install and CAN receive task
#define USE_RTC_SYNC 1   // SNTP time sync (requires USE_WIFI=1)
// USE_MQTT is defined below — it only applies when USE_WIFI=1
// ────────────────────────────────────────────────────────────────

#define SERVER_IP "192.168.1.14"
#define SERVER_PORT 19132

#define USE_MQTT 1

#if USE_MQTT
#define MQTT_URI       "mqtts://5aeaff002e7c423299c2d92361292d54.s1.eu.hivemq.cloud:8883"
#define MQTT_PUB_TOPIC "com/yousef/esp32/data"
extern const char mqtt_root_ca_pem[];
#endif

#define CONNECTIVITY_TEST_IP "8.8.8.8"
#define CONNECTIVITY_TEST_PORT 53
#define CONNECTIVITY_CHECK_INTERVAL_MS 1000
#define CONNECTIVITY_FAIL_THRESHOLD 3
#define CONNECTIVITY_TCP_TIMEOUT_S 3   // Stage 7: TCP send/recv timeout for connectivity probe

// Stage 7: configurable timezone for RTC time sync.
// Egypt (EET = UTC+3). Change to "GMT+1" / "BST" for UK events.
#define TIMEZONE_STR "GMT-3"

#endif // TELEMETRY_CONFIG_H
