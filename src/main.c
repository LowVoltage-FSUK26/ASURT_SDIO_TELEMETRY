//==================================RTOS Libraries Includes==========================//
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "telemetry_config.h"
#include "logging/logging.h"

/* Stage 3: Conditionally include headers based on feature flags.
 * This avoids pulling in driver headers for disabled subsystems. */
#if USE_WIFI
#include "wifi_manager/wifi_manager.h"
#include "connectivity/connectivity.h"
  #if USE_MQTT
  #include "mqtt_sender/mqtt_sender.h"
  #else
  #include "udp_sender/udp_sender.h"
  #endif
#endif

#if USE_CAN
#include "driver/twai.h"
#endif

#if USE_RTC_SYNC
#include "RTC_Time_Sync/rtc_time_sync.h"
#endif

/* Stage 9: LED status indicator + heartbeat diagnostics */
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

/*
 * ================================================================
 * 							SDIO Config Variables
 * ================================================================
 *
 * */

/* Stage 3: SDIO globals only needed when SD card logging is enabled. */
#if USE_SDIO
/* Stage 8: removed unused SDIO_test, SDIO_txt, buffer (were only used by deleted debug block) */
SDIO_FileConfig LOG_CSV;
SDIO_TxBuffer SDIO_buffer;
#endif

/*
 * ================================================================
 * 							CAN Config Variables
 * ================================================================
 *
 * */
/* Stage 3: TWAI (CAN) config structs only compiled when CAN is enabled. */
#if USE_CAN
// 1. Configure TWAI with pins from your ESP32 pinout
twai_general_config_t g_config = {
    .mode = TWAI_MODE_NORMAL,
    .tx_io = CAN_TX_GPIO,  /* Stage 8: centralised in telemetry_config.h */
    .rx_io = CAN_RX_GPIO,  /* Stage 8: centralised in telemetry_config.h */
    .clkout_io = TWAI_IO_UNUSED,
    .bus_off_io = TWAI_IO_UNUSED,
    .tx_queue_len = 5,
    .rx_queue_len = 5,
    .alerts_enabled = TWAI_ALERT_ALL,
    .clkout_divider = 0};
twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();

twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
#endif

/*
 * ================================================================
 * 							RTOS Config Variables
 * ================================================================
 *
 * */
/* Stage 3: Queue handles and task declarations guarded by their respective flags.
 * telemetry_queue bridges CAN → network, so it needs both CAN and WIFI.
 * CAN_SDIO_queue_Handler bridges CAN → SD logging, so it needs both. */
#if USE_CAN
QueueHandle_t CAN_SDIO_queue_Handler;
QueueHandle_t telemetry_queue;
TaskHandle_t CAN_Receive_TaskHandler;
void CAN_Receive_Task_init(void *pvParameters);
#endif

#if USE_SDIO
TaskHandle_t SDIO_Log_TaskHandler;
void SDIO_Log_Task_init(void *pvParameters);
#endif

/* Stage 9: LED status task — blink pattern conveys system state at a glance */
TaskHandle_t LED_Status_TaskHandler;
void led_status_task(void *pvParameters);

/* Stage 9: Heartbeat task — publishes periodic system-health JSON */
#if USE_WIFI
TaskHandle_t Heartbeat_TaskHandler;
void heartbeat_task(void *pvParameters);
#endif

/* Stage 9: Shared diagnostic counters (updated by CAN task, read by heartbeat) */
#if USE_CAN
volatile uint32_t g_can_frame_count = 0;  /* total CAN frames received since boot */
#endif

/* Stage 9: SD card health flag (set false on permanent SDIO failure) */
#if USE_SDIO
volatile bool g_sd_ok = true;
#endif
void app_main(void)
{
   

    //==========================================WIFI Implementation (DONE)===========================================
    /* Stage 3: Wi-Fi init only when USE_WIFI is enabled. */
#if USE_WIFI
    ESP_ERROR_CHECK(wifi_init(WIFI_SSID, WIFI_PASS));

    /* Wait until connected */
    xEventGroupWaitBits(wifi_event_group(), WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);

    ESP_LOGI("WIFI", "Connected to WiFi!");
#endif /* USE_WIFI */

    //==========================================RTC_Time_Sync Implementation (DONE)===========================================
    /* Stage 3: RTC sync requires Wi-Fi for SNTP, so guard with both flags. */
#if USE_WIFI && USE_RTC_SYNC
    Time_Sync_obtain_time();
#endif /* USE_WIFI && USE_RTC_SYNC */

    /* Stage 8: removed debug boot delays */

    //==========================================SDIO Implementation (DONE)===========================================
    /* Stage 3: Entire SDIO init block guarded by USE_SDIO.
     * Stage 5: Re-enabled SD card init and session file naming logic. */
#if USE_SDIO
    static const char *TAG = "SDIO";
    esp_err_t ret;
    ret = SDIO_SD_Init();

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                          "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                          "Make sure SD card lines have pull-up resistors in place.",
                     esp_err_to_name(ret));
        }
    }
    else
    {
        ESP_LOGI(TAG, "Filesystem mounted");

        /* Stage 5: Session file naming — find a fresh LOG_N.CSV slot */
        static char name_buffer[12] = "LOG_0.CSV";
        LOG_CSV.name = name_buffer;
        LOG_CSV.type = CSV;

        snprintf(LOG_CSV.path, sizeof(LOG_CSV.path), "%s/%s", MOUNT_POINT, LOG_CSV.name);

        // Check if the file exists and Modification Time more than 2 days
        // if it exists and last modified was more than 2 days
        //      Increment the name of the file and check again
        // if it exists and last modified in less than 2 days           |
        //      Don't change name and add to the already existing file  |   This logic is implemented
        // if it doesn't exist                                          |   SDIO_SD_Create_Write_File()
        //      Create file                                             |

        struct stat st;
        uint8_t Session_Num = 0;
        while ((stat(LOG_CSV.path, &st) == 0) && (compare_file_time_days(LOG_CSV.path) > MAX_DAYS_MODIFIED))
        {
            // it exists and last modified was more than 2 days
            Session_Num++;
            // Update Name and path
            snprintf(name_buffer, sizeof(name_buffer), "LOG_%u.CSV", Session_Num);
            snprintf(LOG_CSV.path, sizeof(LOG_CSV.path), "%s/%s", MOUNT_POINT, LOG_CSV.name);
        }
    }

    /* Stage 8: removed large commented-out SDIO debug block */
#endif /* USE_SDIO */

    //==========================================RTOS Implementation (Semaphore can be added)===========================================

    //=======================Create Queue====================//

    /* Stage 3: TWAI driver + CAN queues only needed when CAN is enabled. */
#if USE_CAN
    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK)
    {
        ESP_LOGE("Main", "Failed to install TWAI driver");
        return;
    }
    ESP_LOGI("Main", "TWAI Driver installed");

    // Start TWAI driver
    if (twai_start() != ESP_OK)
    {
        ESP_LOGE("Main", "Failed to start TWAI driver");
        return;
    }
    ESP_LOGI("Main", "TWAI Driver started");

    telemetry_queue = xQueueCreate(QUEUE_SIZE, sizeof(twai_message_t));          /* Stage 8: centralised */
    CAN_SDIO_queue_Handler = xQueueCreate(QUEUE_SIZE, sizeof(twai_message_t));  /* Stage 8: centralised */

    if (telemetry_queue == NULL) // If there is no queue created
    {
        ESP_LOGE("RTOS", "Unable to Create Structure Queue\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Stage 5: Re-enabled SDIO queue NULL check */
    if (CAN_SDIO_queue_Handler == NULL) // If there is no queue created
    {
        ESP_LOGE("RTOS", "Unable to Create SDIO Queue");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif /* USE_CAN */

    //=============Define Tasks=================//
    // --- Data Acquisition Tasks on Core 1 ---
    /* Stage 3: CAN receive task only when CAN subsystem is enabled. */
#if USE_CAN
    BaseType_t result_CAN = xTaskCreatePinnedToCore((TaskFunction_t)CAN_Receive_Task_init, "CAN_Receive_Task",
                        TASK_STACK_CAN, NULL, (UBaseType_t)TASK_PRIO_CAN, &CAN_Receive_TaskHandler, 1); /* Stage 8: centralised */
#endif

    /* Stage 5: Re-enabled SDIO log task — needs both SDIO and CAN (data source). */
#if USE_SDIO && USE_CAN
    BaseType_t result_SDIO = xTaskCreatePinnedToCore((TaskFunction_t)SDIO_Log_Task_init, "SDIO_Log_Task",
                        TASK_STACK_SDIO, NULL, (UBaseType_t)TASK_PRIO_SDIO, &SDIO_Log_TaskHandler, 1); /* Stage 8: centralised */
#endif

    // --- Network Tasks on Core 0 ---
    /* Stage 3: All network tasks (MQTT/UDP sender + connectivity monitor)
     * are only meaningful when Wi-Fi is enabled. */
#if USE_WIFI
  #if USE_MQTT
    BaseType_t result_MQT = xTaskCreatePinnedToCore(mqtt_sender_task, "mqtt_sender",
                        TASK_STACK_MQTT, telemetry_queue, TASK_PRIO_NETWORK, NULL, 0); /* Stage 8: centralised */
  #else
    BaseType_t result_UDP = xTaskCreatePinnedToCore(udp_sender_task, "udp_sender",
                        TASK_STACK_UDP, telemetry_queue, TASK_PRIO_NETWORK, NULL, 0); /* Stage 8: centralised */
  #endif
    BaseType_t result_ConMon = xTaskCreatePinnedToCore(connectivity_monitor_task, "conn_monitor",
                        TASK_STACK_CONN_MON, NULL, TASK_PRIO_CONN_MON, NULL, 0); /* Stage 8: centralised */

    /* Stage 9: Heartbeat task — publishes system health every HEARTBEAT_INTERVAL_MS */
    BaseType_t result_HB = xTaskCreatePinnedToCore(heartbeat_task, "heartbeat",
                        TASK_STACK_HEARTBEAT, NULL, TASK_PRIO_HEARTBEAT, &Heartbeat_TaskHandler, 0);
#endif /* USE_WIFI */

    /* Stage 9: LED status indicator task — runs on Core 0 at lowest priority */
    BaseType_t result_LED = xTaskCreatePinnedToCore(led_status_task, "led_status",
                        TASK_STACK_LED, NULL, TASK_PRIO_LED, &LED_Status_TaskHandler, 0);

    printf("========================================\n\n");

    /* Stage 5: Re-enabled SDIO task creation result logging */
#if USE_SDIO && USE_CAN
    if (result_SDIO == pdPASS)
        ESP_LOGI("SDIO_Log_Task", "Task created successfully");
    else
        ESP_LOGE("SDIO_Log_Task", "Task creation failed");
#endif

    /* Stage 3: Task creation result logging matches the guards above. */
#if USE_CAN
    if (result_CAN == pdPASS)
        ESP_LOGI("CAN_Receive_Task", "Task created successfully");
    else
        ESP_LOGE("CAN_Receive_Task", "Task creation failed");
#endif

#if USE_WIFI
  #if USE_MQTT
    if (result_MQT == pdPASS)
        ESP_LOGI("mqtt_sender", "Task created successfully");
    else
        ESP_LOGE("mqtt_sender", "Task creation failed");
  #else
    if (result_UDP == pdPASS)
        ESP_LOGI("udp_sender", "Task created successfully");
    else
        ESP_LOGE("udp_sender", "Task creation failed");
  #endif

    if (result_ConMon == pdPASS)
        ESP_LOGI("conn_monitor", "Task created successfully");
    else
        ESP_LOGE("conn_monitor", "Task creation failed");

    /* Stage 9: Heartbeat task creation result */
    if (result_HB == pdPASS)
        ESP_LOGI("heartbeat", "Task created successfully");
    else
        ESP_LOGE("heartbeat", "Task creation failed");
#endif /* USE_WIFI */

    /* Stage 9: LED task creation result */
    if (result_LED == pdPASS)
        ESP_LOGI("led_status", "Task created successfully");
    else
        ESP_LOGE("led_status", "Task creation failed");

    printf("========================================\n\n");

    /* Stage 9: One-shot stack high-water mark diagnostics — helps right-size stacks.
     * Only compiled when CONFIG_TELEMETRY_DIAG=1 in telemetry_config.h. */
#if CONFIG_TELEMETRY_DIAG
    vTaskDelay(pdMS_TO_TICKS(30000)); /* wait 30 s for tasks to settle */
  #if USE_CAN
    ESP_LOGI("DIAG", "CAN task HWM: %u words",
             (unsigned)uxTaskGetStackHighWaterMark(CAN_Receive_TaskHandler));
  #endif
  #if USE_SDIO && USE_CAN
    ESP_LOGI("DIAG", "SDIO task HWM: %u words",
             (unsigned)uxTaskGetStackHighWaterMark(SDIO_Log_TaskHandler));
  #endif
    ESP_LOGI("DIAG", "LED task HWM: %u words",
             (unsigned)uxTaskGetStackHighWaterMark(LED_Status_TaskHandler));
  #if USE_WIFI
    ESP_LOGI("DIAG", "Heartbeat task HWM: %u words",
             (unsigned)uxTaskGetStackHighWaterMark(Heartbeat_TaskHandler));
  #endif
#endif /* CONFIG_TELEMETRY_DIAG */

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Stage 3: Entire CAN receive task body compiled out when CAN is disabled. */
#if USE_CAN
void CAN_Receive_Task_init(void *pvParameters) // DONE
{
    const char *TAG = "CAN_Receive_Task";
    twai_message_t rx_msg;
    esp_err_t ret;
    uint32_t alerts = 0;
    twai_status_info_t s = {0}; // Initialize the structure to all zeros
    // --- CHANGE START: Add a counter to limit logging frequency ---
    static int timeout_counter = 0;
    // --- CHANGE END ---

    ESP_LOGI("CAN_Receive_Task", "CAN IS WORKING");
    ESP_LOGI("CAN_Receive_Task", "Running on core %d", xPortGetCoreID());
    while (1)
    {
        if (twai_receive(&rx_msg, pdMS_TO_TICKS(20)) == ESP_OK)
        {
            // Reset counter on successful receive
            timeout_counter = 0;

            /* Stage 9: Increment global CAN frame counter for heartbeat diagnostics */
            g_can_frame_count++;

            /* Stage 5: Dual-queue CAN fan-out — non-blocking sends (tick=0)
             * so CAN reception is never stalled by a full queue. */

            /* Send to telemetry (Wi-Fi) queue */
#if USE_WIFI
            xQueueSend(telemetry_queue, &rx_msg, 0);
#endif

            /* Send to SDIO logging queue + notify the SD task */
#if USE_SDIO
            if (xQueueSend(CAN_SDIO_queue_Handler, &rx_msg, 0) != pdPASS)
            {
                /* Queue full — notify SD task to drain it */
                if (SDIO_Log_TaskHandler != NULL)
                {
                    xTaskNotifyGive(SDIO_Log_TaskHandler);
                }
            }
#endif
        }
        else
        {
            // --- CHANGE START: Only log periodically on timeout ---
            timeout_counter++;
            if (timeout_counter >= 100)
            {                        // Log roughly once per second
                timeout_counter = 0; // Reset counter
                ESP_LOGW(TAG, "No message received within the timeout period");
                ret = twai_read_alerts(&alerts, 0);
                if (ret == ESP_OK)
                {
                    ESP_LOGI(TAG, "TWAI alert: %08ld", alerts);
                }
                twai_get_status_info(&s);
                ESP_LOGI(TAG, "RX errors: %ld, bus errors: %ld, RX queue full: %ld",
                         s.rx_error_counter, s.bus_error_count, s.rx_missed_count);
            }
            // --- CHANGE END ---
        }
        // vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif /* USE_CAN */

/* Stage 5: Re-enabled SDIO_Log_Task_init — receives CAN frames from
 * CAN_SDIO_queue_Handler, aggregates one sample of each CAN ID within
 * a 50 ms window, then appends the row to the CSV on the SD card.
 * Switch-case IDs verified against COMM_CAN_ID_t enum in logging.h. */
#if USE_SDIO && USE_CAN
void SDIO_Log_Task_init(void *pvParameters)
{
    const char *TAG = "SDIO_Log_Task";
    ESP_LOGI(TAG, "SDIO_LOG IS WORKING");
    ESP_LOGI(TAG, "Running on core %d", xPortGetCoreID());
    uint8_t prev_reset = 0;

    // Assign Zero to all elements of SDIO_buffer and Log initial Line
    EMPTY_SDIO_BUFFER(SDIO_buffer);

    if (SDIO_SD_Create_Write_File(&LOG_CSV, &SDIO_buffer) == ESP_OK)
        ESP_LOGI(TAG, "%s Written Successfully!", LOG_CSV.name);

    twai_message_t buffer;
    const uint8_t NUM_IDS = COMM_CAN_ID_COUNT; /* Stage 5: 6 IDs — matches COMM_CAN_ID_t */
    uint8_t id_received[NUM_IDS];
    TickType_t period = pdMS_TO_TICKS(50);

    while (1)
    {
        /* Stage 5: Block until CAN task notifies us (queue was full or periodic) */
        /* Wake every 50 ms regardless of whether the queue overflowed.
         * This ensures logging happens at low CAN traffic (bench tests,
         * run startup) and not only when CAN_SDIO_queue_Handler is full. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        // 1. Clear buffer and flags
        EMPTY_SDIO_BUFFER(SDIO_buffer);
        memset(&buffer, 0, sizeof(twai_message_t));
        memset(id_received, 0, sizeof(id_received));

        TickType_t start = xTaskGetTickCount();
        TickType_t now = start;
        uint8_t received_count = 0;

        /* Stage 5: Drain CAN_SDIO_queue_Handler within the 50 ms window,
         * collecting one sample per CAN ID. */
        while ((now - start) < period && received_count < NUM_IDS)
        {
            TickType_t remaining = period - (now - start);
            if (xQueueReceive(CAN_SDIO_queue_Handler, &buffer, remaining))
            {
                /* Stage 5: Demux by CAN ID — verified against COMM_CAN_ID_t enum:
                 * 0x071 IMU_ANGLE, 0x072 IMU_ACCEL, 0x073 ADC,
                 * 0x074 PROX_ENCODER, 0x075 GPS_LATLONG, 0x076 TEMP */
                switch (buffer.identifier)
                {
                case COMM_CAN_ID_IMU_ANGLE:
                    if (id_received[COMM_CAN_ID_IMU_ANGLE - COMM_CAN_ID_FISRT] == 0)
                    {
                        memcpy(&SDIO_buffer.imu_ang, buffer.data, sizeof(COMM_message_IMU_t));
                        id_received[COMM_CAN_ID_IMU_ANGLE - COMM_CAN_ID_FISRT] = 1;
                        received_count++;
                    }
                    break;

                case COMM_CAN_ID_IMU_ACCEL:
                    if (id_received[COMM_CAN_ID_IMU_ACCEL - COMM_CAN_ID_FISRT] == 0)
                    {
                        memcpy(&SDIO_buffer.imu_accel, buffer.data, sizeof(COMM_message_IMU_t));
                        id_received[COMM_CAN_ID_IMU_ACCEL - COMM_CAN_ID_FISRT] = 1;
                        received_count++;
                    }
                    break;

                case COMM_CAN_ID_ADC:
                    if (id_received[COMM_CAN_ID_ADC - COMM_CAN_ID_FISRT] == 0)
                    {
                        memcpy(&SDIO_buffer.adc, buffer.data, sizeof(COMM_message_ADC_t));
                        id_received[COMM_CAN_ID_ADC - COMM_CAN_ID_FISRT] = 1;
                        received_count++;
                    }
                    break;

                case COMM_CAN_ID_PROX_ENCODER:
                    if (id_received[COMM_CAN_ID_PROX_ENCODER - COMM_CAN_ID_FISRT] == 0)
                    {
                        memcpy(&SDIO_buffer.prox_encoder, buffer.data, sizeof(COMM_message_PROX_encoder_t));
                        id_received[COMM_CAN_ID_PROX_ENCODER - COMM_CAN_ID_FISRT] = 1;
                        received_count++;
                    }
                    break;

                case COMM_CAN_ID_GPS_LATLONG:
                    if (id_received[COMM_CAN_ID_GPS_LATLONG - COMM_CAN_ID_FISRT] == 0)
                    {
                        memcpy(&SDIO_buffer.gps, buffer.data, sizeof(COMM_message_GPS_t));
                        id_received[COMM_CAN_ID_GPS_LATLONG - COMM_CAN_ID_FISRT] = 1;
                        received_count++;
                    }
                    break;

                case COMM_CAN_ID_TEMP:
                    if (id_received[COMM_CAN_ID_TEMP - COMM_CAN_ID_FISRT] == 0)
                    {
                        memcpy(&SDIO_buffer.temp, buffer.data, sizeof(COMM_message_Temp_t));
                        id_received[COMM_CAN_ID_TEMP - COMM_CAN_ID_FISRT] = 1;
                        received_count++;
                    }
                    break;

                default:
                    break;
                }
            }
            now = xTaskGetTickCount();
        }

        /* Stage 5: Append aggregated row to SD card CSV.
         * If logging fails 2 consecutive times, give up and park the task. */
        if (SDIO_SD_Add_Data(&LOG_CSV, &SDIO_buffer) != ESP_OK)
        {
            if (prev_reset < 2)
            {
                esp_err_t ret;
                SDIO_SD_DeInit();

                vTaskDelay(pdMS_TO_TICKS(2000));

                ret = SDIO_SD_Init();
                if (ret != ESP_OK)
                {
                    prev_reset++;
                    ESP_LOGI(TAG, "ERROR! : %s is not Created // Appended", LOG_CSV.name);
                    if (ret == ESP_FAIL)
                    {
                        ESP_LOGE(TAG, "Failed to mount filesystem. "
                                      "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                                      "Make sure SD card lines have pull-up resistors in place.",
                                 esp_err_to_name(ret));
                    }
                }
                else
                {
                    ESP_LOGI(TAG, "Filesystem mounted");
                    prev_reset = 0;
                }
            }
            else
            {
                /* Stage 5: Permanent failure — unmount and park the task */
                SDIO_SD_DeInit();
                /* Stage 9: Signal permanent SD failure to heartbeat/LED tasks */
                g_sd_ok = false;
                while (1)
                {
                    ESP_LOGE(TAG, "SDIO_Logging is Down");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
            }
        }
        else
        {
            /* Stage 5: Success — reset failure counter */
            prev_reset = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}
#endif /* USE_SDIO && USE_CAN */

/* ================================================================
 * Stage 9: LED status indicator task
 * ================================================================
 * Blink patterns communicate system state without requiring a serial console:
 *   Fast blink  (100 ms) : Wi-Fi connecting
 *   Slow blink  (1000 ms): Wi-Fi connected, system nominal
 *   Double-blink          : SD card error
 *   Solid on              : CAN receiving data (toggled on each frame batch)
 */
void led_status_task(void *pvParameters)
{
    const char *TAG = "led_status";
    ESP_LOGI(TAG, "LED status task running on core %d", xPortGetCoreID());

    /* Stage 9: Configure LED_GPIO as push-pull output */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);

#if USE_CAN
    uint32_t prev_can_count = 0; /* Stage 9: track CAN activity */
#endif

    while (1)
    {
#if USE_SDIO
        /* Stage 9: Double-blink pattern when SD card has permanently failed */
        if (!g_sd_ok)
        {
            /* Double blink: ON-OFF-ON-OFF then pause */
            gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(80));
            gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(80));
            gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(80));
            gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(760));
            continue;
        }
#endif

#if USE_CAN
        /* Stage 9: Solid on while CAN data is actively flowing */
        if (g_can_frame_count != prev_can_count)
        {
            prev_can_count = g_can_frame_count;
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
#endif

#if USE_WIFI
        /* Stage 9: Fast blink while Wi-Fi is not yet connected */
        if ((xEventGroupGetBits(wifi_event_group()) & WIFI_CONNECTED_BIT) == 0)
        {
            gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
#endif

        /* Stage 9: Slow blink — system nominal */
        gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(900));
    }
}

/* ================================================================
 * Stage 9: Heartbeat task — publishes periodic system-health JSON
 * ================================================================
 * Published every HEARTBEAT_INTERVAL_MS with uptime, free heap,
 * SD card status, and CAN frames-per-second.
 * Uses MQTT (topic + "/heartbeat") or UDP depending on USE_MQTT.
 */
#if USE_WIFI
void heartbeat_task(void *pvParameters)
{
    const char *TAG = "heartbeat";
    ESP_LOGI(TAG, "Heartbeat task running on core %d", xPortGetCoreID());

    EventGroupHandle_t eg = wifi_event_group();

    /* Stage 9: Wait for Wi-Fi before attempting any network I/O */
    xEventGroupWaitBits(eg, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

#if USE_CAN
    uint32_t prev_can_count = 0;
#endif

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));

        /* Stage 9: Skip if Wi-Fi is currently down */
        if ((xEventGroupGetBits(eg) & WIFI_CONNECTED_BIT) == 0)
            continue;

        /* Stage 9: Compute CAN frames per second */
        uint32_t can_fps = 0;
#if USE_CAN
        uint32_t current_count = g_can_frame_count;
        can_fps = (current_count - prev_can_count) * 1000 / HEARTBEAT_INTERVAL_MS;
        prev_can_count = current_count;
#endif

        /* Stage 9: Build compact JSON heartbeat payload (~80 bytes) */
        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        int sd_ok_flag = 1;
#if USE_SDIO
        sd_ok_flag = g_sd_ok ? 1 : 0;
#endif

        char json[96];
        snprintf(json, sizeof(json),
                 "{\"uptime_s\":%lu,\"free_heap\":%lu,\"sd_ok\":%d,\"can_fps\":%lu}",
                 (unsigned long)uptime_s,
                 (unsigned long)free_heap,
                 sd_ok_flag,
                 (unsigned long)can_fps);

        /* Stage 9: Publish via MQTT or send as UDP packet */
#if USE_MQTT
        /* Note: the MQTT client handle lives inside mqtt_sender_task.
         * We re-use the public topic with a "/heartbeat" suffix.
         * A minimal approach: publish via a local MQTT client init. */
        {
            extern esp_mqtt_client_handle_t mqtt_heartbeat_client;
            if (mqtt_heartbeat_client != NULL)
            {
                esp_mqtt_client_publish(mqtt_heartbeat_client, MQTT_PUB_TOPIC "/heartbeat",
                                        json, 0, 0, 0);
            }
        }
#else
        /* Stage 9: Re-use the UDP socket for heartbeat packets */
        {
            extern void udp_send_heartbeat(const char *data, int len);
            udp_send_heartbeat(json, (int)strlen(json));
        }
#endif

        ESP_LOGD(TAG, "Heartbeat: %s", json);
    }
}
#endif /* USE_WIFI */
