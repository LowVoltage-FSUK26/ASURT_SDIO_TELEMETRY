# ASURT SDIO Telemetry System

> **Formula Student UK — Embedded Data Logging & Live Telemetry**
> Firmware for the ESP32-S3 onboard the ASURT Formula Student car.
> Acquires vehicle sensor data over CAN bus, logs it to an SD card via SDIO, and streams it live to a pit-lane server over Wi-Fi (UDP or MQTT over TLS).

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
- [FreeRTOS Task Map](#freertos-task-map)
- [Configuration](#configuration)
  - [Wi-Fi Credentials](#wi-fi-credentials)
  - [Telemetry Mode — UDP vs MQTT](#telemetry-mode--udp-vs-mqtt)
  - [MQTT Settings](#mqtt-settings)
  - [Connectivity Monitor](#connectivity-monitor)
- [Build & Flash](#build--flash)
  - [Prerequisites](#prerequisites)
  - [Clone & Build](#clone--build)
  - [Flash & Monitor](#flash--monitor)
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
3. **Streams** raw CAN frames in real-time to a pit-lane laptop over **Wi-Fi**, selectable between bare **UDP** and **MQTT over TLS**.

Time-stamping is handled by synchronising the ESP32's internal RTC to **NTP** immediately after Wi-Fi connects, so every log entry carries a wall-clock timestamp accurate to the second.

---

## System Architecture

```
┌─────────────────────────────────────────────────────────┐
│                        ESP32-S3                         │
│                                                         │
│  CAN Bus (125k)                                         │
│  ──────────────► CAN_Receive_Task (Core 1, Pri 4)       │
│                        │                                │
│               FreeRTOS Queue (x2)                       │
│                 ┌───────┴────────┐                      │
│                 ▼                ▼                       │
│   SDIO_Log_Task (Core 1)   telemetry_queue              │
│   → SD Card (.CSV)              │                       │
│                          ┌──────┴───────┐               │
│                          ▼              ▼               │
│               mqtt_sender_task    udp_sender_task        │
│               (Core 0, Pri 3)    (Core 0, Pri 3)        │
│               [USE_MQTT=1]       [USE_MQTT=0]           │
│                                                         │
│  connectivity_monitor_task (Core 0, Pri 3)              │
│  → Polls 8.8.8.8:53 every 1 s, forces reconnect        │
│    after 3 consecutive failures                         │
│                                                         │
│  wifi_manager  ← exponential back-off auto-reconnect   │
│  rtc_time_sync ← SNTP on pool.ntp.org (GMT+3)          │
└─────────────────────────────────────────────────────────┘
              │                          │
         ─────┼── Wi-Fi ─────────────────┼──────
              │                          │
        Pit Laptop                  HiveMQ Cloud
        (UDP rx)              (MQTT broker, TLS 8883)
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
| D2 | 19 |
| D3 | 20 |

> **Note:** The code also contains pin definitions for **ESP-WROOM-32** under the `#ifdef ESP_WROOM_32` guard. Switch the define in `logging.h` if porting to that module.

#### CAN Bus (TWAI)

| Signal | GPIO |
|---|---|
| TX | 41 |
| RX | 42 |

> A CAN transceiver IC (e.g. SN65HVD230) is required between the ESP32 and the bus.

#### Miscellaneous

| Signal | GPIO |
|---|---|
| Status LED | 2 |

---

## Sensor Data & CAN IDs

All sensor nodes on the car broadcast on a **125 kbit/s CAN bus**. The telemetry node accepts all frames (`TWAI_FILTER_CONFIG_ACCEPT_ALL`).

| CAN ID | Constant | Payload Struct | Contents |
|---|---|---|---|
| `0x071` | `COMM_CAN_ID_IMU_ANGLE` | `COMM_message_IMU_t` | IMU orientation X / Y / Z (16-bit each) |
| `0x072` | `COMM_CAN_ID_IMU_ACCEL` | `COMM_message_IMU_t` | IMU acceleration X / Y / Z (16-bit each) |
| `0x073` | `COMM_CAN_ID_ADC` | `COMM_message_ADC_t` | SUS 1–4 (10-bit), Pressure 1–2 (10-bit) |
| `0x074` | `COMM_CAN_ID_PROX_ENCODER` | `COMM_message_PROX_encoder_t` | RPM FL/FR/RL/RR (11-bit), Encoder angle (10-bit), Speed km/h (8-bit) |
| `0x075` | `COMM_CAN_ID_GPS_LATLONG` | `COMM_message_GPS_t` | Latitude / Longitude (float) |
| `0x076` | `COMM_CAN_ID_TEMP` | `COMM_message_Temp_t` | Tyre temp FL/FR/RL/RR (16-bit each) |

> When adding new IDs, see the **[Adding a New CAN ID](#adding-a-new-can-id)** section.

---

## Project Structure

```
ASURT_SDIO_TELEMETRY/
├── .pio/                        # PlatformIO build artefacts (git-ignored)
├── .vscode/                     # Editor settings
├── include/                     # Global headers (if any)
├── lib/                         # External libraries
├── src/
│   ├── connectivity/
│   │   ├── connectivity.c       # TCP connectivity probe + Wi-Fi reconnect trigger
│   │   └── connectivity.h
│   ├── Logging/
│   │   ├── logging.c            # SDIO / SD card driver, CSV & TXT file management
│   │   └── logging.h            # CAN ID enums, sensor structs, SDIO API
│   ├── mqtt_sender/
│   │   ├── mqtt_sender.c        # MQTT over TLS client task (HiveMQ)
│   │   └── mqtt_sender.h
│   ├── RTC_Time_Sync/
│   │   ├── rtc_time_sync.c      # SNTP init, time string helper
│   │   └── rtc_time_sync.h
│   ├── udp_sender/
│   │   ├── udp_sender.c         # UDP sender task with retry & socket recovery
│   │   └── udp_sender.h
│   ├── wifi_manager/
│   │   ├── wifi_manager.c       # Wi-Fi STA init, event handler, exponential back-off
│   │   └── wifi_manager.h
│   ├── CMakeLists.txt
│   ├── main.c                   # App entry point, RTOS task creation
│   └── telemetry_config.h       # ⚠️  All runtime config lives here
├── test/
├── .gitignore
├── CMakeLists.txt
├── platformio.ini
└── sdkconfig.esp32-s3-devkitc-1
```

---

## Module Reference

### wifi\_manager

**Files:** `src/wifi_manager/wifi_manager.c/.h`

Initialises the ESP32 in **Station mode** and manages the full connection lifecycle. It exposes a FreeRTOS `EventGroupHandle_t` that all other tasks block on before touching the network.

Key behaviours:
- Exponential back-off reconnect timer: starts at `500 ms`, doubles on each failure, caps at `10 000 ms`.
- Clears `WIFI_CONNECTED_BIT` on disconnect or IP loss; sets it on `IP_EVENT_STA_GOT_IP`.
- Mutex-protected `wifi_get_ip_info()` for safe concurrent reads.
- `wifi_force_reconnect()` — called by the connectivity monitor to drop and re-establish the association.

```c
// Minimal usage
ESP_ERROR_CHECK(wifi_init("MY_SSID", "MY_PASSWORD"));
xEventGroupWaitBits(wifi_event_group(), WIFI_CONNECTED_BIT,
                    pdFALSE, pdFALSE, portMAX_DELAY);
```

---

### connectivity

**Files:** `src/connectivity/connectivity.c/.h`

Runs a background task that periodically proves **end-to-end internet reachability** (not just association) by opening a TCP connection to `8.8.8.8:53`.

- Checks every `CONNECTIVITY_CHECK_INTERVAL_MS` (default 1 000 ms).
- After `CONNECTIVITY_FAIL_THRESHOLD` (default 3) consecutive failures, calls `wifi_force_reconnect()`.
- Logs remaining attempts at `WARN` level; logs recovery at `INFO`.

---

### rtc\_time\_sync

**Files:** `src/RTC_Time_Sync/rtc_time_sync.c/.h`

Synchronises the system clock via **SNTP** immediately after Wi-Fi connects.

| Function | Description |
|---|---|
| `Time_Sync_init_sntp()` | Configures SNTP in poll mode against `pool.ntp.org` |
| `Time_Sync_obtain_time()` | Calls init, waits up to 10 s for sync, sets `TZ=GMT-3` |
| `Time_Sync_get_rtc_time_str(buf, len)` | Fills `buf` with `"YYYY-MM-DD HH:MM:SS"` from the local clock |

> **Timezone:** Currently hardcoded to `GMT-3`. Update the `setenv("TZ", ...)` call in `rtc_time_sync.c` for your event location (e.g. `GMT+1` for the UK).

---

### logging (SDIO)

**Files:** `src/Logging/logging.c/.h`

Full SD card driver built on top of ESP-IDF's **FATFS + SDMMC** stack using the **4-bit SDIO** bus.

#### Public API

| Function | Description |
|---|---|
| `SDIO_SD_Init()` | Mounts the SD card filesystem at `/sdcard` |
| `SDIO_SD_DeInit()` | Flushes and unmounts |
| `SDIO_SD_Create_Write_File(cfg, buf)` | Creates a new `.CSV` or `.TXT` file and writes the first row |
| `SDIO_SD_Add_Data(cfg, buf)` | Appends a data row to an existing file |
| `SDIO_SD_Read_Data(cfg)` | Reads and prints file contents (debug) |
| `SDIO_SD_Close_file()` | Closes the currently open file handle |
| `SDIO_SD_LOG_CAN_Message(msg)` | Decodes a raw `twai_message_t` and appends to the active CSV |
| `compare_file_time_days(path)` | Returns the number of days since a file was last modified |

#### File Naming & Session Management

Log files are named `LOG_0.CSV`, `LOG_1.CSV`, … The startup logic increments the session number if the previous log file was last modified more than `MAX_DAYS_MODIFIED` (2) days ago. This keeps each race day's data in a separate file.

#### CSV Column Layout

```
Timestamp, SUS1, SUS2, SUS3, SUS4, P1, P2,
RPM_FL, RPM_FR, RPM_RL, RPM_RR, ENC_ANGLE, SPEED,
IMU_ANG_X, IMU_ANG_Y, IMU_ANG_Z,
IMU_ACC_X, IMU_ACC_Y, IMU_ACC_Z,
TEMP_FL, TEMP_FR, TEMP_RL, TEMP_RR,
GPS_LAT, GPS_LON
```

---

### udp\_sender

**Files:** `src/udp_sender/udp_sender.c/.h`

Streams raw `twai_message_t` frames over **UDP** to the pit laptop.

Features:
- Blocks on `WIFI_CONNECTED_BIT` before creating the socket.
- Up to `UDP_MAX_RETRIES` (4) retries with exponential back-off (`10 ms` base, doubling).
- Automatically re-initialises the socket after a Wi-Fi reconnect.
- `udp_socket_close()` is safe to call from any context; protected by a mutex.
- Queued messages are replaced by newer ones during retries to always send the latest data.

---

### mqtt\_sender

**Files:** `src/mqtt_sender/mqtt_sender.c/.h`

Streams raw `twai_message_t` frames over **MQTT** (QoS 0) to HiveMQ Cloud.

Features:
- TLS with the embedded **ISRG Root X1** certificate (Let's Encrypt chain).
- Waits for both `WIFI_CONNECTED_BIT` **and** `mqtt_connected` before publishing.
- Logs a single warning when the connection is lost; suppresses repeated spam.
- Compiled out entirely when `USE_MQTT 0` — the task exits immediately and is deleted.

---

## FreeRTOS Task Map

| Task | Core | Priority | Stack | Purpose |
|---|---|---|---|---|
| `CAN_Receive_Task` | 1 | 4 | 4096 B | TWAI receive → telemetry queue |
| `SDIO_Log_Task` | 1 | 3 | 4096 B | Queue → SD card CSV *(currently disabled)* |
| `mqtt_sender_task` | 0 | 3 | 4096 B | telemetry queue → MQTT (when `USE_MQTT=1`) |
| `udp_sender_task` | 0 | 3 | 4096 B | telemetry queue → UDP (when `USE_MQTT=0`) |
| `connectivity_monitor_task` | 0 | 3 | 4096 B | Periodic TCP probe, reconnect trigger |

**Queue depths:** Both `telemetry_queue` and `CAN_SDIO_queue_Handler` are created with a depth of **10** frames (`sizeof(twai_message_t)` each).

**Core assignment rationale:** CAN acquisition runs on Core 1 to isolate it from the Wi-Fi stack (which runs on Core 0), reducing jitter in the receive loop.

---

## Configuration

All runtime parameters live in a single file:

### `src/telemetry_config.h`

```c
// ── UDP Target ──────────────────────────────────────────
#define SERVER_IP   "192.168.1.14"   // Pit laptop IP
#define SERVER_PORT  19132           // Pit laptop UDP port

// ── Transport selection ─────────────────────────────────
#define USE_MQTT 1                   // 1 = MQTT over TLS, 0 = bare UDP

// ── MQTT (HiveMQ Cloud) ─────────────────────────────────
#define MQTT_URI       "mqtts://XXXXXXXXXXXXXXXX.s1.eu.hivemq.cloud:8883"
#define MQTT_USER      "your_username"
#define MQTT_PASS      "your_password"
#define MQTT_PUB_TOPIC "com/yousef/esp32/data"

// ── Connectivity monitor ─────────────────────────────────
#define CONNECTIVITY_TEST_IP        "8.8.8.8"
#define CONNECTIVITY_TEST_PORT       53
#define CONNECTIVITY_CHECK_INTERVAL_MS  1000
#define CONNECTIVITY_FAIL_THRESHOLD     3
```

### Wi-Fi Credentials

Wi-Fi SSID and password are passed directly to `wifi_init()` in `main.c`:

```c
ESP_ERROR_CHECK(wifi_init("YOUR_SSID", "YOUR_PASSWORD"));
```

> **⚠️ Security Warning:** Do not commit real credentials. Consider moving them to `menuconfig` secrets or a `credentials.h` that is listed in `.gitignore`.

### Telemetry Mode — UDP vs MQTT

| `USE_MQTT` | Active task | Notes |
|---|---|---|
| `1` | `mqtt_sender_task` | TLS encrypted, routed via cloud broker |
| `0` | `udp_sender_task` | Direct LAN, lowest latency |

### MQTT Settings

The TLS CA certificate is embedded directly in `mqtt_sender.c` as `mqtt_root_ca_pem[]` (ISRG Root X1 / Let's Encrypt). This certificate expires **2035-06-04** — update before that date.

### Connectivity Monitor

| Macro | Default | Effect |
|---|---|---|
| `CONNECTIVITY_CHECK_INTERVAL_MS` | `1000` | Period between TCP probes |
| `CONNECTIVITY_FAIL_THRESHOLD` | `3` | Consecutive failures before reconnect |
| `CONNECTIVITY_TEST_IP` | `"8.8.8.8"` | Probe target IP |
| `CONNECTIVITY_TEST_PORT` | `53` | Probe target port |

---

## Build & Flash

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html) (CLI or VS Code extension)
- ESP-IDF toolchain (installed automatically by PlatformIO)
- USB cable connected to the ESP32-S3 DevKitC-1

### Clone & Build

```bash
git clone https://github.com/ASURT/<repo-name>.git
cd ASURT_SDIO_TELEMETRY

# Edit credentials before building
nano src/telemetry_config.h   # set SERVER_IP, MQTT_USER, MQTT_PASS
nano src/main.c               # set wifi_init("SSID", "PASS")

pio run
```

### Flash & Monitor

```bash
# Flash firmware
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor

# Flash + monitor in one command
pio run --target upload && pio device monitor
```

Expected boot log:

```
I (xxx) wifi_manager: Wi-Fi initialization complete
I (xxx) wifi_manager: Got IP: 192.168.x.x
I (xxx) WIFI: Connected to WiFi!
I (xxx) rtc_time: Initializing SNTP
I (xxx) Main: TWAI Driver installed
I (xxx) Main: TWAI Driver started
I (xxx) CAN_Receive_Task: Task created successfully
I (xxx) mqtt_sender: Task created successfully
I (xxx) conn_monitor: Task created successfully
I (xxx) mqtt_sender: MQTT connected
```

---

## Extending the System

### Adding a New CAN ID

Follow these steps when a new sensor node joins the bus:

1. **`logging.h`** — Add the new ID to `COMM_CAN_ID_t` enum and increment `COMM_CAN_ID_COUNT`.
2. **`logging.h`** — Define a new payload struct (e.g. `COMM_message_NewSensor_t`).
3. **`logging.h`** — Add a field of that struct type to `SDIO_TxBuffer`.
4. **`logging.h`** — Extend the `EMPTY_SDIO_BUFFER` macro to zero the new field.
5. **`logging.c`** — Add the new column(s) to the CSV format strings in `SDIO_SD_Add_Data` and `SDIO_SD_Create_Write_File`.
6. **`main.c`** — Add a `case` for the new ID in the `SDIO_Log_Task_init` switch statement.

### Switching Transport Layer

Change one line in `telemetry_config.h`:

```c
#define USE_MQTT 0   // switches from mqtt_sender_task → udp_sender_task at compile time
```

Both tasks consume from the same `telemetry_queue` and are pinned to Core 0, so no other changes are required.

---

## Known Issues & TODOs

| Status | Item |
|---|---|
| 🔴 Disabled | `SDIO_Log_Task` is commented out — SD logging is not active in the current build. Re-enable and test the CSV logging pipeline end-to-end. |
| 🟡 Hardcoded | Wi-Fi credentials are set directly in `main.c`. Move to `menuconfig` or a gitignored header. |
| 🟡 Hardcoded | Timezone is set to `GMT-3` in `rtc_time_sync.c`. Update to `GMT+1` (BST) for UK events. |
| 🟡 Hardcoded | MQTT broker URL and credentials are in `telemetry_config.h` which may be committed. |
| 🟠 Incomplete | `udp_sender_task` — heap monitor (free heap logging) is commented out. Enable for memory profiling on long runs. |
| 🟠 Incomplete | Session file naming logic (`LOG_N.CSV` increment) in `main.c` is commented out. Needs to be restored for correct multi-session logging. |
| 🔵 Future | Add a **reception acknowledgement** / heartbeat on the pit-side to confirm telemetry is live before the car leaves the pits. |
| 🔵 Future | Consider **dual-core queue separation**: give SDIO its own queue so a slow SD write never starves the telemetry sender. |
| 🔵 Future | Add **CRC or sequence number** to UDP packets to allow the pit laptop to detect dropped frames. |

---

## Authors

| Name | Role |
|---|---|
| **Mina Fathy** | SDIO / SD card driver, RTC time sync module |
| **Yousef** | Wi-Fi manager, MQTT sender, UDP sender, connectivity monitor |

**Team:** ASURT (Ain Shams University Racing Team) — Formula Student UK

---

*Last updated: 2025 · ESP-IDF via PlatformIO · ESP32-S3-DevKitC-1*
