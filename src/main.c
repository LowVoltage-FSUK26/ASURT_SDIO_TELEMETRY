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

#define LED_GPIO 2 // GPIO pin for the LED
#define Queue_Size 10

/*
 * ================================================================
 * 							SDIO Config Variables
 * ================================================================
 *
 * */

/* Stage 3: SDIO globals only needed when SD card logging is enabled. */
#if USE_SDIO
// Debug Varibales
SDIO_FileConfig SDIO_test;
SDIO_FileConfig SDIO_txt;
SDIO_TxBuffer buffer;
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
    .tx_io = GPIO_NUM_41, // CAN TX on GPIO1
    .rx_io = GPIO_NUM_42, // CAN RX on GPIO2
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

    //@debug
    ESP_LOGI("Main", "Initializing");
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI("Main", ".......");
    vTaskDelay(pdMS_TO_TICKS(3000));

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
        char name_buffer[12] = "LOG_0.CSV";
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

    /*
    //@debug SDIO

    SDIO_txt.name = "Test2.TXT";
    SDIO_txt.type = TXT;
    buffer.string = "Hello World line 1\r\nHello World line 2\r\nHello World line 3\r";
    if (SDIO_SD_Create_Write_File(&SDIO_txt, &buffer) == ESP_OK)
    {
        ESP_LOGI(TAG, "%s Written Successfully!", SDIO_txt.name);
    }

    LOG_CSV.name = "LOG_1.CSV";
    LOG_CSV.type = CSV;
    SDIO_buffer.string = "LOG1";
    SDIO_buffer.timestamp = "2023-10-01 12:00:00";
    SDIO_buffer.adc.SUS_1 = 15;
    SDIO_buffer.adc.SUS_2 = 20;
    SDIO_buffer.adc.SUS_3 = 25;
    SDIO_buffer.adc.SUS_4 = 30;
    SDIO_buffer.adc.PRESSURE_1 = 10;
    SDIO_buffer.adc.PRESSURE_2 = 15;
    SDIO_buffer.prox_encoder.RPM_front_left = 1000;
    SDIO_buffer.prox_encoder.RPM_front_right = 1100;
    SDIO_buffer.prox_encoder.RPM_rear_left = 1200;
    SDIO_buffer.prox_encoder.RPM_rear_right = 1300;
    SDIO_buffer.prox_encoder.ENCODER_angle = 45;
    SDIO_buffer.imu_accel.x = 100;
    SDIO_buffer.imu_accel.y = 200;
    SDIO_buffer.imu_accel.z = 300;

    if (SDIO_SD_Create_Write_File(&LOG_CSV, &SDIO_buffer) == ESP_OK)
    {
        ESP_LOGI(TAG, "%s Written Successfully!", LOG_CSV.name);
    }

    // SDIO_SD_Read_Data(&SDIO_txt);
    // SDIO_SD_Read_Data(&LOG_CSV);

    // Append data to the existing files
    buffer.string = "Hello World line 4\r\nHello World line 5\r\n";
    if (SDIO_SD_Add_Data(&SDIO_txt, &buffer) == ESP_OK)
    {
        ESP_LOGI(TAG, "TEST.TXT Appended Successfully!");
    }

    // Assign Zero to all elements of SDIO_buffer
    EMPTY_SDIO_BUFFER(SDIO_buffer);

    if (SDIO_SD_Add_Data(&LOG_CSV, &SDIO_buffer) == ESP_OK)
    {
        ESP_LOGI(TAG, "%s Appended Successfully!", LOG_CSV.name);
    }

    if (SDIO_SD_Close_file() == ESP_OK)
    {
        ESP_LOGI(TAG, "File Closed Successfully!");
    }

    // Read the files again to verify the appended data
    SDIO_SD_Read_Data(&SDIO_txt);
    vTaskDelay(pdMS_TO_TICKS(1000));
    SDIO_SD_Read_Data(&LOG_CSV);

    // twai_message_t rx_msg;
    // SDIO_SD_LOG_CAN_Message(&rx_msg);

    // All done, unmount partition and disable SDMMC peripheral
    if (SDIO_SD_DeInit() == ESP_OK)
        ESP_LOGI(TAG, "Card unmounted successfully"); */
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

    telemetry_queue = xQueueCreate(Queue_Size, sizeof(twai_message_t));
    CAN_SDIO_queue_Handler = xQueueCreate(Queue_Size, sizeof(twai_message_t));

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
    BaseType_t result_CAN = xTaskCreatePinnedToCore((TaskFunction_t)CAN_Receive_Task_init, "CAN_Receive_Task", 4096, NULL, (UBaseType_t)4, &CAN_Receive_TaskHandler, 1); // <-- Priority 4
#endif

    /* Stage 5: Re-enabled SDIO log task — needs both SDIO and CAN (data source). */
#if USE_SDIO && USE_CAN
    BaseType_t result_SDIO = xTaskCreatePinnedToCore((TaskFunction_t)SDIO_Log_Task_init, "SDIO_Log_Task", 4096, NULL, (UBaseType_t)3, &SDIO_Log_TaskHandler, 1); // <-- Core 1, Priority 3
#endif

    // --- Network Tasks on Core 0 ---
    /* Stage 3: All network tasks (MQTT/UDP sender + connectivity monitor)
     * are only meaningful when Wi-Fi is enabled. */
#if USE_WIFI
  #if USE_MQTT
    BaseType_t result_MQT = xTaskCreatePinnedToCore(mqtt_sender_task, "mqtt_sender", 4096, telemetry_queue, 3, NULL, 0); // <-- Core 0
  #else
    BaseType_t result_UDP = xTaskCreatePinnedToCore(udp_sender_task, "udp_sender", 4096, telemetry_queue, 3, NULL, 0); // <-- Core 0
  #endif
    BaseType_t result_ConMon = xTaskCreatePinnedToCore(connectivity_monitor_task, "conn_monitor", 4096, NULL, 3, NULL, 0); // <-- Core 0
#endif /* USE_WIFI */

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
#endif /* USE_WIFI */

    printf("========================================\n\n");

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
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

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
