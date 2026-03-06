# ASURT SDIO Telemetry System

> **Formula Student UK — Embedded Data Logging & Live Telemetry**
> Firmware for the ESP32-S3 onboard the ASURT Formula Student car.
> Acquires vehicle sensor data over CAN bus, logs it to an SD card via 4-bit SDIO, and streams it live to a pit-lane server over Wi-Fi (UDP or MQTT over TLS).

---

## Table of Contents

- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Hardware](#hardware)
  - [Target MCU](#target-mcu)
  - [Pin Assignments](#pin-assignments)
- [Sensor Data & CAN IDs](#sensor-data--can-ids)
- [Project Structure](#project-structure)
- [Module Reference](#module-reference)
  - [wifi\_manager](#wifi_manager)
  - [connectivity](#connectivity)
  - [rtc\_time\_sync](#rtc_time_sync)
  - [logging (SDIO)](#logging-sdio)
  - [udp\_sender](#udp_sender)
  - [mqtt\_sender](#mqtt_sender)
  - [led\_status\_task](#led_status_task)
  - [heartbeat\_task](#heartbeat_task)
- [Wire Format — telemetry\_packet\_t](#wire-format--telemetry_packet_t)
- [FreeRTOS Task Map](#freertos-task-map)
- [Configuration](#configuration)
  - [Credentials](#credentials)
  - [Feature Flags](#feature-flags)
  - [Telemetry Mode — UDP vs MQTT](#telemetry-mode--udp-vs-mqtt)
  - [MQTT Settings](#mqtt-settings)
  - [Connectivity Monitor](#connectivity-monitor)
  - [Diagnostic Flags](#diagnostic-flags)
- [Build & Flash](#build--flash)
  - [Prerequisites](#prerequisites)
  - [Clone & Configure](#clone--configure)
  - [Build, Flash & Monitor](#build-flash--monitor)
  - [Expected Boot Log](#expected-boot-log)
- [Extending the System](#extending-the-system)
  - [Adding a New CAN ID](#adding-a-new-can-id)
  - [Switching Transport Layer](#switching-transport-layer)
- [Known Issues & TODOs](#known-issues--todos)
- [Authors](#authors)

---

## Overview

The ASURT Telemetry System is a dual-purpose embedded firmware running on an **ESP32-S3**. During a competition run or test session it simultaneously:

1. **Receives** all vehicle sensor data frames broadcast over the car's **CAN bus** (125 kbit/s).
2. **Logs** structured records to a **microSD card** (via 4-bit SDIO) in timestamped `.CSV` files for post-session analysis.
3. **Streams** CAN frames in real-time to a pit-lane laptop over **Wi-Fi**, selectable between bare **UDP** and **MQTT over TLS**.
4. **Publishes periodic heartbeat JSON** containing uptime, free heap, SD card health, and CAN frames-per-second — visible on both MQTT and UDP transports.
5. **Signals system state** via an on-board LED using distinct blink patterns.

Time-stamping is handled by synchronising the ESP32's internal RTC to **NTP** immediately after Wi-Fi connects, so every log entry carries a wall-clock timestamp accurate to the millisecond.

---

## System Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                           ESP32-S3                             │
│                                                                │
│  CAN Bus (125k)                                                │
│  ──────────────► CAN_Receive_Task (Core 1, Pri 4)             │
│                        │                                       │
│               ┌────────┴────────┐                              │
│               ▼                 ▼                              │
│   CAN_SDIO_queue_Handler   telemetry_queue                     │
│               │                 │                              │
│               ▼                 ├──────────────────┐           │
│   SDIO_Log_Task (Core 1)        ▼                  ▼           │
│   → SD Card (LOG_N.CSV)  mqtt_sender_task   udp_sender_task    │
│                           (Core 0, Pri 3)   (Core 0, Pri 3)   │
│                           [USE_MQTT=1]      [USE_MQTT=0]       │
│                                                                │
│  connectivity_monitor_task (Core 0, Pri 2)                     │
│  → Polls 8.8.8.8:53 every 1 s, forces reconnect               │
│    after 3 consecutive failures                                │
│                                                                │
│  heartbeat_task (Core 0, Pri 2)                                │
│  → JSON {uptime_s, free_heap, sd_ok, can_fps} every 5 s       │
│    via MQTT topic + "/heartbeat", or UDP socket                │
│                                                                │
│  led_status_task (Core 0, Pri 1)                               │
│  → Blink pattern reflects live system state                    │
│                                                                │
│  wifi_manager  ← exponential back-off auto-reconnect          │
│  rtc_time_sync ← SNTP on pool.ntp.org (GMT-3 / EET)           │
└────────────────────────────────────────────────────────────────┘
              │                            │
         ─────┼── Wi-Fi ───────────────────┼──────
              │                            │
        Pit Laptop                    HiveMQ Cloud
    (UDP rx port 19132)          (MQTT broker, TLS 8883)
```

---

## Hardware

### Target MCU

| Property | Value |
|---|---|
| Module | ESP32-S3-DevKitC-1 |
| Framework | ESP-IDF (via PlatformIO) |
| SDK config | `sdkconfig.esp32-s3-devkitc-1` |

### Pin Assignments

#### SDIO — MicroSD Card (4-bit bus)

| Signal | GPIO |
|---|---|
| CMD | 35 |
| CLK | 36 |
| D0 | 37 |
| D1 | 38 |
| D2 | 21 |
| D3 | 47 |

> **Note:** All SDIO lines require **10 kΩ external pull-up resistors**. Internal pull-ups are enabled in firmware but are insufficient on their own. Bus width is configurable via `#define SDMMC_BUS_WIDTH_4` in `logging.h`; remove it to fall back to 1-bit mode.

#### CAN Bus (TWAI)

| Signal | GPIO |
|---|---|
| TX | 41 |
| RX | 42 |

> A CAN transceiver IC (e.g. SN65HVD230) is required between the ESP32 and the physical bus. Defined in `telemetry_config.h` as `CAN_TX_GPIO` / `CAN_RX_GPIO`.

#### Miscellaneous

| Signal | GPIO | Notes |
|---|---|---|
| Status LED | 2 | On-board LED on most ESP32-S3 devkits. Defined as `LED_GPIO`. |

---

## Sensor Data & CAN IDs

All sensor nodes broadcast on a **125 kbit/s CAN bus**. The telemetry node accepts all frames (`TWAI_FILTER_CONFIG_ACCEPT_ALL()`).

| CAN ID | Constant | Payload Struct | Contents |
|---|---|---|---|
| `0x071` | `COMM_CAN_ID_IMU_ANGLE` | `COMM_message_IMU_t` | IMU orientation X / Y / Z (16-bit each) |
| `0x072` | `COMM_CAN_ID_IMU_ACCEL` | `COMM_message_IMU_t` | IMU acceleration X / Y / Z (16-bit each) |
| `0x073` | `COMM_CAN_ID_ADC` | `COMM_message_ADC_t` | SUS 1–4 (10-bit), Pressure 1–2 (10-bit) |
| `0x074` | `COMM_CAN_ID_PROX_ENCODER` | `COMM_message_PROX_encoder_t` | RPM FL/FR/RL/RR (11-bit), Encoder angle (10-bit), Speed km/h (8-bit) |
| `0x075` | `COMM_CAN_ID_GPS_LATLONG` | `COMM_message_GPS_t` | Latitude / Longitude (float) |
| `0x076` | `COMM_CAN_ID_TEMP` | `COMM_message_Temp_t` | Tyre temp FL/FR/RL/RR (16-bit each) |

> When adding new IDs, follow the checklist in **[Adding a New CAN ID](#adding-a-new-can-id)**.

---

## Project Structure

```
ASURT_SDIO_TELEMETRY/
├── .pio/                        # PlatformIO build artefacts (git-ignored)
├── .vscode/                     # Editor settings
├── include/
├── lib/
├── src/
│   ├── connectivity/
│   │   ├── connectivity.c       # TCP probe + Wi-Fi reconnect trigger
│   │   └── connectivity.h
│   ├── Logging/
│   │   ├── logging.c            # SDIO / SD card driver, CSV & raw CAN logging
│   │   └── logging.h            # CAN ID enums, sensor structs, SDIO API
│   ├── mqtt_sender/
│   │   ├── mqtt_sender.c        # MQTT over TLS task + shared client handle
│   │   └── mqtt_sender.h
│   ├── RTC_Time_Sync/
│   │   ├── rtc_time_sync.c      # SNTP init, ms-resolution time string
│   │   └── rtc_time_sync.h
│   ├── udp_sender/
│   │   ├── udp_sender.c         # UDP sender with retry, mutex socket, heartbeat helper
│   │   └── udp_sender.h
│   ├── wifi_manager/
│   │   ├── wifi_manager.c       # Wi-Fi STA, event handler, exponential back-off
│   │   └── wifi_manager.h
│   ├── CMakeLists.txt
│   ├── credentials.h            # ⚠️  Git-ignored — copy from credentials.h.example
│   ├── credentials.h.example    # Template: fill in SSID, password, MQTT creds
│   ├── main.c                   # App entry point, task creation, LED & heartbeat tasks
│   ├── telemetry_config.h       # ⚠️  All runtime config and feature flags live here
│   └── telemetry_packet.h       # Stable 20-byte wire format for UDP/MQTT payloads
├── test/
├── .gitignore
├── CMakeLists.txt
├── DECODER_MIGRATION_NOTES.md
├── platformio.ini
└── sdkconfig.esp32-s3-devkitc-1
```

---

## Module Reference

### wifi\_manager

**Files:** `src/wifi_manager/wifi_manager.c/.h`

Initialises the ESP32 in **Station mode** and manages the full connection lifecycle. All other tasks block on the exported `EventGroupHandle_t` before touching the network.

Key behaviours:

- Exponential back-off reconnect timer: starts at 500 ms, doubles on each failure, caps at 10 000 ms.
- Clears `WIFI_CONNECTED_BIT` on `STA_DISCONNECTED` or `IP_LOST`; sets it on `IP_EVENT_STA_GOT_IP`.
- Mutex-protected `wifi_get_ip_info()` for safe concurrent reads from any task.
- `wifi_force_reconnect()` — called by the connectivity monitor to drop and re-associate.

```c
// Minimal usage
ESP_ERROR_CHECK(wifi_init(WIFI_SSID, WIFI_PASS));
xEventGroupWaitBits(wifi_event_group(), WIFI_CONNECTED_BIT,
                    pdFALSE, pdFALSE, portMAX_DELAY);
```

---

### connectivity

**Files:** `src/connectivity/connectivity.c/.h`

Background task that proves **end-to-end internet reachability** — not just association — by attempting a TCP connection to `8.8.8.8:53`.

- Checks every `CONNECTIVITY_CHECK_INTERVAL_MS` (default 1 000 ms).
- The TCP probe has a configurable send/receive timeout (`CONNECTIVITY_TCP_TIMEOUT_S`, default 3 s) so it can never block indefinitely.
- After `CONNECTIVITY_FAIL_THRESHOLD` (default 3) consecutive failures, calls `wifi_force_reconnect()`.
- Logs remaining attempts at `WARN` level and restoration at `INFO`.

---

### rtc\_time\_sync

**Files:** `src/RTC_Time_Sync/rtc_time_sync.c/.h`

Synchronises the system clock via **SNTP** immediately after Wi-Fi connects.

| Function | Description |
|---|---|
| `Time_Sync_init_sntp()` | Configures SNTP in poll mode against `pool.ntp.org` |
| `Time_Sync_obtain_time()` | Calls init, waits up to 10 s for sync, then applies the configured timezone |
| `Time_Sync_get_rtc_time_str(buf, len)` | Fills `buf` with `"YYYY-MM-DD HH:MM:SS.mmm"` (millisecond resolution via `esp_timer_get_time()`) |

> **Timezone:** Configured in `telemetry_config.h` as `TIMEZONE_STR`. Currently set to `"GMT-3"` (Egypt, EET/UTC+3). Change to `"GMT+1"` for UK Formula Student events. Follows POSIX sign convention — `GMT-3` means +3 hours from UTC.

---

### logging (SDIO)

**Files:** `src/Logging/logging.c/.h`

Full SD card driver built on ESP-IDF's **FATFS + SDMMC** stack using the **4-bit SDIO** bus.

#### Public API

| Function | Description |
|---|---|
| `SDIO_SD_Init()` | Mounts the SD card filesystem at `/sdcard` |
| `SDIO_SD_DeInit()` | Flushes, closes any open handle, and unmounts |
| `SDIO_SD_Create_Write_File(cfg, buf)` | Creates a new `.CSV` or `.TXT` file (or appends if it exists and is less than `MAX_DAYS_MODIFIED` days old) |
| `SDIO_SD_Add_Data(cfg, buf)` | Appends a data row; flushes/closes the handle every `LOG_FLUSH_EVERY_N_WRITES` (default 5) writes |
| `SDIO_SD_Close_file()` | Explicitly closes the currently open file handle |
| `SDIO_SD_log_can_message_to_csv(msg)` | Appends a raw `twai_message_t` to `SDIO_CAN.CSV` in a separate debug format |
| `compare_file_time_days(path)` | Returns days since a file was last modified (used for session rotation) |
| `SDIO_SD_Read_Data(cfg)` | Dumps file contents over UART — **debug only**, compile-guarded by `CONFIG_SDIO_DEBUG_READ` |

#### Session File Naming

Log files are named `LOG_0.CSV`, `LOG_1.CSV`, and so on. At startup the firmware checks whether the most recent file was modified more than `MAX_DAYS_MODIFIED` days ago (default 2). If so, it increments the session number, ensuring each race day's data lands in a separate file.

#### CSV Column Layout

```
Timestamp, Label,
SUS_1_raw, SUS_2_raw, SUS_3_raw, SUS_4_raw,
PRESSURE_1_raw, PRESSURE_2_raw,
RPM_FL, RPM_FR, RPM_RL, RPM_RR, ENC_ANGLE_raw, SPEED_kmh,
IMU_Ang_X, IMU_Ang_Y, IMU_Ang_Z,
IMU_Accel_X, IMU_Accel_Y, IMU_Accel_Z,
Temp_FL_raw, Temp_FR_raw, Temp_RL_raw, Temp_RR_raw,
GPS_Lat, GPS_Lon
```

#### SDIO Log Task Behaviour

`SDIO_Log_Task_init` runs on Core 1 and blocks on `ulTaskNotifyTake` with a 50 ms timeout — it wakes either when the CAN receive task sends a notification (queue was full) or when the 50 ms timeout elapses, whichever comes first. Within each window it drains `CAN_SDIO_queue_Handler`, collecting the first sample of each CAN ID. It then appends the aggregated row to the CSV and sleeps for an additional 60 ms (`vTaskDelay`), giving an effective minimum cycle time of roughly 110 ms. On write failure it attempts a full `DeInit` → `Init` cycle twice before flagging `g_sd_ok = false` and parking itself.

---

### udp\_sender

**Files:** `src/udp_sender/udp_sender.c/.h`

Streams `telemetry_packet_t` frames over **UDP** to the pit laptop.

- Blocks on `WIFI_CONNECTED_BIT` before creating the socket.
- Drains the queue to the newest frame before each send to minimise latency under load.
- Up to `UDP_MAX_RETRIES` (4) retries with exponential back-off (base 10 ms, doubling).
- Automatically re-initialises the socket on Wi-Fi reconnect.
- `udp_socket_close()` is mutex-protected and safe to call from any context.
- `udp_send_heartbeat(data, len)` — reuses the same mutex-protected socket for heartbeat JSON from the heartbeat task, avoiding a second socket.
- Optional heap monitoring every 1 000 send cycles, enabled via `CONFIG_TELEMETRY_DEBUG_HEAP`.

---

### mqtt\_sender

**Files:** `src/mqtt_sender/mqtt_sender.c/.h`

Streams `telemetry_packet_t` frames over **MQTT** (QoS 0) to HiveMQ Cloud.

- TLS with the embedded **ISRG Root X1** certificate (Let's Encrypt chain, expires 2035-06-04).
- Waits for `WIFI_CONNECTED_BIT` **and** `mqtt_connected` before publishing; logs a timeout warning every 10 s while the broker is unreachable.
- Exposes `mqtt_heartbeat_client` (a public `esp_mqtt_client_handle_t`) so `heartbeat_task` can publish on the same TLS session without creating a second connection.
- Compiled out entirely when `USE_MQTT 0`.

---

### led\_status\_task

**Files:** `src/main.c`

Runs on Core 0 at lowest priority (1). Drives `LED_GPIO` (GPIO 2) with blink patterns that communicate system state without requiring a serial console.

| Pattern | Meaning |
|---|---|
| Fast blink (100 ms on / 100 ms off) | Wi-Fi not yet connected |
| 200 ms ON pulse per loop iteration | CAN data actively flowing |
| Slow blink (100 ms on / 900 ms off) | Wi-Fi connected, system nominal |
| Double-blink (80 ms on/off × 2) then 760 ms pause | SD card permanently failed (`g_sd_ok = false`) |

> **Priority order:** SD failure → CAN activity → Wi-Fi disconnected → nominal. The first matching condition wins on each loop iteration.

---

### heartbeat\_task

**Files:** `src/main.c`

Runs on Core 0. Publishes a compact JSON packet every `HEARTBEAT_INTERVAL_MS` (default 5 000 ms). Skips silently when Wi-Fi is down.

**Payload format (~80 bytes):**
```json
{"uptime_s":123,"free_heap":187432,"sd_ok":1,"can_fps":42}
```

When `USE_MQTT=1` the payload is published to `MQTT_PUB_TOPIC/heartbeat` on the shared `mqtt_heartbeat_client` handle. When `USE_MQTT=0` it is sent over the shared UDP socket via `udp_send_heartbeat()`.

---

## Wire Format — telemetry\_packet\_t

Both UDP and MQTT senders convert `twai_message_t` into a fixed **20-byte** packed struct before transmission. This gives the receiver a stable layout and sequence numbers for drop detection.

```c
typedef struct __attribute__((packed)) {
    uint32_t seq;           // rolling sequence number
    uint32_t timestamp_ms;  // ms since boot (esp_timer_get_time / 1000)
    uint16_t can_id;        // CAN identifier
    uint8_t  dlc;           // data length code (0–8)
    uint8_t  flags;         // bit 0 = extd, bit 1 = rtr
    uint8_t  data[8];       // raw CAN payload
} telemetry_packet_t;       // 4+4+2+1+1+8 = 20 bytes
```

The heartbeat task sends plain JSON and does **not** use this struct.

---

## FreeRTOS Task Map

| Task | Core | Priority | Stack | Queue |
|---|---|---|---|---|
| `CAN_Receive_Task` | 1 | 4 | 4 096 B | Produces → `telemetry_queue` and `CAN_SDIO_queue_Handler` |
| `SDIO_Log_Task` | 1 | 3 | 6 144 B | Consumes `CAN_SDIO_queue_Handler` |
| `mqtt_sender_task` | 0 | 3 | 6 144 B | Consumes `telemetry_queue` (when `USE_MQTT=1`) |
| `udp_sender_task` | 0 | 3 | 4 096 B | Consumes `telemetry_queue` (when `USE_MQTT=0`) |
| `connectivity_monitor_task` | 0 | 2 | 3 072 B | — |
| `heartbeat_task` | 0 | 2 | 4 096 B | — |
| `led_status_task` | 0 | 1 | 2 048 B | — |

Both queues hold **10** frames of `sizeof(twai_message_t)` each (`QUEUE_SIZE`).

**Core assignment rationale:** CAN acquisition and SD logging run on Core 1 to isolate them from the Wi-Fi/TCP-IP stack (Core 0), minimising jitter in the receive loop.

---

## Configuration

All runtime parameters are centralised in two files:

### Credentials

Copy `credentials.h.example` to `credentials.h` (which is git-ignored) and fill in your values:

```c
#define WIFI_SSID   "YOUR_WIFI_SSID"
#define WIFI_PASS   "YOUR_WIFI_PASSWORD"
#define MQTT_USER   "YOUR_MQTT_USERNAME"
#define MQTT_PASS   "YOUR_MQTT_PASSWORD"
```

`telemetry_config.h` includes `credentials.h` automatically.

> **⚠️ Security:** Never commit `credentials.h`. It is already listed in `.gitignore`. The provided `credentials.h.example` contains only placeholder values and is safe to commit.

### Feature Flags

Defined at the top of `telemetry_config.h`. Set any flag to `0` to compile out that subsystem entirely, which is useful for bench testing without all hardware attached.

| Flag | Default | Effect |
|---|---|---|
| `USE_WIFI` | `1` | Wi-Fi init, all network tasks, connectivity monitor |
| `USE_SDIO` | `1` | SD card init and SDIO logging task |
| `USE_CAN` | `1` | TWAI driver install and CAN receive task |
| `USE_RTC_SYNC` | `1` | SNTP time synchronisation (requires `USE_WIFI=1`) |
| `USE_MQTT` | `1` | MQTT sender instead of UDP (requires `USE_WIFI=1`) |

### Telemetry Mode — UDP vs MQTT

| `USE_MQTT` | Active sender task | Notes |
|---|---|---|
| `1` | `mqtt_sender_task` | TLS-encrypted, routed via HiveMQ Cloud broker |
| `0` | `udp_sender_task` | Direct LAN, lowest latency |

Heartbeat JSON is published via the active transport automatically.

### MQTT Settings

```c
#define MQTT_URI       "mqtts://<broker-id>.s1.eu.hivemq.cloud:8883"
#define MQTT_PUB_TOPIC "com/yousef/esp32/data"
```

The embedded **ISRG Root X1** TLS certificate in `mqtt_sender.c` expires **2035-06-04** — remember to update it before that date.

### Connectivity Monitor

```c
#define CONNECTIVITY_TEST_IP          "8.8.8.8"
#define CONNECTIVITY_TEST_PORT         53
#define CONNECTIVITY_CHECK_INTERVAL_MS 1000
#define CONNECTIVITY_FAIL_THRESHOLD    3
#define CONNECTIVITY_TCP_TIMEOUT_S     3
```

### Diagnostic Flags

These are off by default and should only be enabled during development:

```c
// Log free heap every ~1 000 UDP sends
#define CONFIG_TELEMETRY_DEBUG_HEAP  0

// Log stack high-water marks for all tasks 30 s after boot
#define CONFIG_TELEMETRY_DIAG        0

// Compile in SDIO_SD_Read_Data() — dumps file contents over UART (slow, debug only)
#define CONFIG_SDIO_DEBUG_READ       0
```

---

## Build & Flash

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html) (CLI or VS Code extension)
- ESP-IDF toolchain (installed automatically by PlatformIO)
- USB-C cable connected to the ESP32-S3-DevKitC-1

### Clone & Configure

```bash
git clone https://github.com/ASURT/<repo-name>.git
cd ASURT_SDIO_TELEMETRY

# Create your credentials file from the template
cp src/credentials.h.example src/credentials.h
# Edit src/credentials.h — fill in WIFI_SSID, WIFI_PASS, MQTT_USER, MQTT_PASS

# Adjust pit laptop IP / transport mode if needed
# Edit src/telemetry_config.h
```

### Build, Flash & Monitor

```bash
# Build only
pio run

# Flash firmware
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor

# Flash + monitor in one step
pio run --target upload && pio device monitor
```

### Expected Boot Log

```
I (xxx) wifi_manager: Wi-Fi initialization complete
I (xxx) wifi_manager: Got IP: 192.168.x.x
I (xxx) WIFI: Connected to WiFi!
I (xxx) rtc_time: Initializing SNTP
I (xxx) SDIO: Filesystem mounted
I (xxx) Main: TWAI Driver installed
I (xxx) Main: TWAI Driver started
I (xxx) SDIO_Log_Task: Task created successfully
I (xxx) CAN_Receive_Task: Task created successfully
I (xxx) mqtt_sender: Task created successfully
I (xxx) conn_monitor: Task created successfully
I (xxx) heartbeat: Task created successfully
I (xxx) led_status: Task created successfully
I (xxx) mqtt_sender: MQTT connected
```

---

## Extending the System

### Adding a New CAN ID

Follow all six steps — skipping any one will cause silent data loss or a build error.

1. **`logging.h`** — Add the new ID to the `COMM_CAN_ID_t` enum and increment `COMM_CAN_ID_COUNT`.
2. **`logging.h`** — Define a new payload struct (e.g. `COMM_message_NewSensor_t`).
3. **`logging.h`** — Add a field of that struct type to `SDIO_TxBuffer`.
4. **`logging.h`** — Extend the `EMPTY_SDIO_BUFFER` macro to zero the new fields.
5. **`logging.c`** — Add the new column(s) to the CSV header and data format strings in both `SDIO_SD_Add_Data` and `SDIO_SD_Create_Write_File`.
6. **`main.c`** — Add a `case` for the new ID in the `SDIO_Log_Task_init` switch statement.

### Switching Transport Layer

Change one line in `telemetry_config.h`:

```c
#define USE_MQTT 0   // 0 = udp_sender_task, 1 = mqtt_sender_task
```

Both tasks consume from the same `telemetry_queue` and are pinned to Core 0. No other changes are required — the heartbeat task switches transports automatically.

---

## Known Issues & TODOs

| Status | Item |
|---|---|
| 🟡 Security | `credentials.h` is git-ignored, but `MQTT_URI` and `MQTT_PUB_TOPIC` are still in `telemetry_config.h`, which may be committed. Consider moving them into `credentials.h` as well. |
| 🟡 Timezone | `TIMEZONE_STR` is currently `"GMT-3"` (Egypt). Change to `"GMT+1"` (BST) for UK Formula Student events. Note POSIX sign convention: `GMT-3` = UTC+3. |
| 🟠 Incomplete | `SDIO_SD_log_can_message_to_csv()` creates a separate `SDIO_CAN.CSV` for raw CAN debug logging, but is not wired into any task by default. Wire it in for raw-frame debugging sessions. |
| 🟠 Incomplete | `CONFIG_TELEMETRY_DEBUG_HEAP` and `CONFIG_TELEMETRY_DIAG` are both `0`. Enable them during long-run memory and stack profiling, then disable before competition. |
| 🔵 Future | Add **dual-core queue separation**: give SDIO its own independent queue depth tuning so a slow SD write never starves the telemetry sender queue. |
| 🔵 Future | Consider **pit-side acknowledgement**: the heartbeat JSON currently flows one-way. A lightweight ACK back to the car would confirm live reception before the car leaves the pits. |
| 🔵 Future | The MQTT TLS certificate (`mqtt_root_ca_pem`) expires **2035-06-04**. Set a calendar reminder to rotate it. |

---

## Authors

| Name | Role |
|---|---|
| **Mina Fathy** | SDIO / SD card driver (`logging.c/.h`), RTC time sync module |
| **Yousef** | Wi-Fi manager, MQTT sender, UDP sender, connectivity monitor, heartbeat & LED tasks |

**Team:** ASURT (Ain Shams University Racing Team) — Formula Student UK

---

*Last updated: 2026 · ESP-IDF via PlatformIO · ESP32-S3-DevKitC-1*
