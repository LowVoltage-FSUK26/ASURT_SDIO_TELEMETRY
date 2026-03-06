#ifndef TELEMETRY_CONFIG_H
#define TELEMETRY_CONFIG_H

#include "credentials.h"
#include <driver/gpio.h>  /* Stage 8: needed for GPIO_NUM_xx in CAN pin defines */

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

// ── Stage 8: Centralised hardware & RTOS config ────────────────

// CAN bus GPIO pins
#define CAN_TX_GPIO             GPIO_NUM_41
#define CAN_RX_GPIO             GPIO_NUM_42

// RTOS task stack sizes (bytes)
#define TASK_STACK_CAN          4096
#define TASK_STACK_SDIO         6144
#define TASK_STACK_MQTT         6144
#define TASK_STACK_UDP          4096
#define TASK_STACK_CONN_MON     3072

// RTOS task priorities
#define TASK_PRIO_CAN           4
#define TASK_PRIO_NETWORK       3
#define TASK_PRIO_SDIO          3
#define TASK_PRIO_CONN_MON      2

// FreeRTOS queue depth
#define QUEUE_SIZE              10
#define QUEUE_SIZE_SDIO         20   // deeper buffer — absorbs SD write latency spikes

// SD logging: flush to disk every N writes
#define LOG_FLUSH_EVERY_N_WRITES   5

// SD logging: max age (days) before rotating to a new session file
#define MAX_DAYS_MODIFIED          2

// ── Stage 9: Quality-of-life & diagnostic config ───────────────

// LED status indicator GPIO (on-board LED on most ESP32 devkits)
#define LED_GPIO                GPIO_NUM_2

// RTOS config for new Stage 9 tasks
#define TASK_STACK_LED          2048
#define TASK_STACK_HEARTBEAT    4096
#define TASK_PRIO_LED           1      // lowest priority — purely cosmetic
#define TASK_PRIO_HEARTBEAT     2

// Heartbeat publish interval (ms)
#define HEARTBEAT_INTERVAL_MS   5000

// Set to 1 during development to log heap usage in udp_sender
#define CONFIG_TELEMETRY_DEBUG_HEAP  0

// Set to 1 during development to log stack high-water marks 30 s after boot
#define CONFIG_TELEMETRY_DIAG       0

// ────────────────────────────────────────────────────────────────

#endif // TELEMETRY_CONFIG_H
