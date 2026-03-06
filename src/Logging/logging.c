/*
 * Logging.c
 *
 *  Created on: Jun 22, 2025
 *      Author: Mina Fathy
 *      Description: This file contains the implementation of the SD Card Logging API.
 *      Note: This API is designed to work with the ESP32 platform and uses the ESP-IDF framework.
 *             Use POSIX and C standard library functions to work with files:
 */

#include "logging.h"
#include "..\RTC_Time_Sync\rtc_time_sync.h"

/*
 * ================================================================
 * 							SD Card Variables
 * ================================================================
 *
 * */
sdmmc_card_t *card;                     /* Holds Mounted SD Card infromation*/
const char mount_point[] = MOUNT_POINT; /* Holds the data path of the SD card*/
FILE *f = NULL;                         /* File object for SD */

/*
 * ================================================================
 * 							File I/O Variables
 * ================================================================
 *
 * */
static esp_err_t ret;           /* Fatfas functions common result code */
static char *open_file = NULL;  /* Holds the name of Currently opened file */
uint32_t bytewritten, byteread; /* File Write/Read counters */
uint8_t writes_Num = 0;

/*
 * ================================================================
 * 					API Functions Definition
 * ================================================================
 *
 * */

/**================================================================
 * @Fn				- SDIO_SD_Init
 * @breif			- Initializes SD Cards & prints card Info
 * @param [in]		- None
 * @retval			- Value indicates the States of SD Card (Anything other that ESP_OK is an Error)
 * Note				- None
 */
esp_err_t SDIO_SD_Init(void)
{
    ret = ESP_OK;

    // Options for mounting the filesystem.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 4096}; // used to be 16 * 1024

    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 40MHz for SDMMC)
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
#if SDMMC_SPEED_HS
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#elif SDMMC_SPEED_UHS_I_SDR50
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_SDR50;
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
#elif SDMMC_SPEED_UHS_I_DDR50
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DDR50;
#endif

    // ESP32-S3     pin	SD card     pin	Notes
    // GPIO36	    CLK	            10K pullup
    // GPIO35	    CMD	            10k pullup
    // GPIO37	    D0	            10k pullup
    // GPIO38	    D1	            not used in 1-line SD mode; 10k pullup in 4-line mode
    // GPIO21	    D2	            not used in 1-line SD mode; 10k pullup in 4-line mode
    // GPIO47	    D3	            not used in 1-line SD mode, but card's D3 pin must have a 10k pullup

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // Note: Due to differences in D2, D3, we set it manually

    slot_config.clk = GPIO_NUM_36;
    slot_config.cmd = GPIO_NUM_35;
    slot_config.d0 = GPIO_NUM_37;
    slot_config.d1 = GPIO_NUM_38;
    slot_config.d2 = GPIO_NUM_21;
    slot_config.d3 = GPIO_NUM_47;

    // Set bus width to use:
#ifdef SDMMC_BUS_WIDTH_4
    slot_config.width = 4;
#else
    slot_config.width = 1;
#endif

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, 10k external pullups are recommended.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    // Set the log level for the GPIO driver to WARN to reduce Messages
    // esp_log_level_set("gpio", ESP_LOG_WARN);
    // esp_vfs_fat_sdcard_unmount(mount_point, card);

    // ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);
    return ret;
}

/**================================================================
 * @Fn				- SDIO_SD_DeInit
 * @breif			- De-Initializes SD Cards
 * @param [in]		- None
 * @retval			- Value indicates the States of SD Card (Anything other that ESP_OK is an Error)
 * Note				- None
 */
esp_err_t SDIO_SD_DeInit(void)
{
    ret = ESP_OK;
    if (open_file != NULL && f != NULL)
        fclose(f);
    ret = esp_vfs_fat_sdcard_unmount(mount_point, card);
    return ret;
}

/**================================================================
 * @Fn				- SDIO_SD_Create_Write_File
 * @breif			- Creates New File and Writes Data in File
 * @param [in]		- file: To select the name and extenstion of File
 * @param [in]		- pTxBuffer: Pointer to buffer storing data to be stored
 * @retval			- Value indicates the States of SD Card (Anything other that ESP_OK is an Error)
 * Note				- For pTxBuffer: .TXT File Types Config -> String only
 * 									 .CSV File Types Config -> f_printf format
 */
esp_err_t SDIO_SD_Create_Write_File(SDIO_FileConfig *file, SDIO_TxBuffer *pTxBuffer)
{
    ret = ESP_OK;
    static const char *TAG = "SDIO_SD_Create_Write_File";
    // Check if another file is already opened
    if (open_file != NULL && f != NULL)
    {
        fclose(f);        // close the previously opened file
        open_file = NULL; // Reset the open file name
        f = NULL;
    }
    snprintf(file->path, sizeof(file->path), "%s/%s", MOUNT_POINT, file->name);

    // Get current Time
    char time_buffer[32];
    if (Time_Sync_get_rtc_time_str(time_buffer, sizeof(time_buffer)) != true)
    {
        ESP_LOGE("RTC", "Failed to get time.");
        strcpy(time_buffer, "XXXX-XX-XX XX:XX:XX");
    }

    // Check if the files exists and Modification Time less than 2 days
    struct stat st;
    if ((stat(file->path, &st) == 0) && (compare_file_time_days(file->path) <= MAX_DAYS_MODIFIED))
    {
        //@debgu
        // ESP_LOGI(TAG, "File is recent");
        // Add to the file and don't create new one
        if (SDIO_SD_Add_Data(file, pTxBuffer) != ESP_OK)
        {
            ESP_LOGE("SDIO", "Error in %s Append!", file->name);
        }
    }
    else // Create new file
    {
        //@Debug
        // ESP_LOGI(TAG, "File is not recent or don't exits\n Creating new File!");

        f = fopen(file->path, "w");
        if (f == NULL)
        {
            ESP_LOGE("SDIO", "Error in %s Create Unable to Create Path:%s!", file->name, file->path);

            return ESP_FAIL; // Failed to open file for writing
        }
        open_file = file->name; // Assign the name of the opened file

        // Check if file type is .TXT or .CSV
        if (file->type == TXT)
        {
            // Write string to file
            bytewritten = fprintf(f, "%s\n", pTxBuffer->string);
            if ((bytewritten == 0) || ret != ESP_OK)
            {
                ret = ESP_ERR_NOT_FINISHED;
                ESP_LOGE("SDIO", "ESP_ERR_NOT_FINISHED in Writing %s file!", file->name);
                return ret; // Failed to write to file
            }
        }
        else if (file->type == CSV)
        {

            // Write CSV header to file
            fprintf(f, "Timestamp,Label,"
                       "SUS_1,SUS_2,SUS_3,SUS_4,"
                       "PRESSURE_1,PRESSURE_2,"
                       "RPM_FL,RPM_FR,RPM_RL,RPM_RR,"
                       "ENC_ANGLE,"
                       "IMU_Ang_X,IMU_Ang_Y,IMU_Ang_Z,"
                       "IMU_Accel_X,IMU_Accel_Y,IMU_Accel_Z,"
                       "Temp_FL,Temp_FR,Temp_RL,Temp_RR,"
                       "GPS_Long, GPS_Lat\n");

            // Write formatted data to file
            bytewritten = fprintf(f, "%s,%s,"
                                     "%u,%u,%u,%u,"
                                     "%u,%u,"
                                     "%u,%u,%u,%u,"
                                     "%u,"
                                     "%u,%u,%u,"
                                     "%u,%u,%u,"
                                     "%u,%u,%u,%u,"
                                     "%f,%f\n",

                                  time_buffer,
                                  pTxBuffer->string,

                                  pTxBuffer->adc.SUS_1,
                                  pTxBuffer->adc.SUS_2,
                                  pTxBuffer->adc.SUS_3,
                                  pTxBuffer->adc.SUS_4,
                                  pTxBuffer->adc.PRESSURE_1,
                                  pTxBuffer->adc.PRESSURE_2,

                                  pTxBuffer->prox_encoder.RPM_front_left,
                                  pTxBuffer->prox_encoder.RPM_front_right,
                                  pTxBuffer->prox_encoder.RPM_rear_left,
                                  pTxBuffer->prox_encoder.RPM_rear_right,
                                  pTxBuffer->prox_encoder.ENCODER_angle,

                                  pTxBuffer->imu_ang.x,
                                  pTxBuffer->imu_ang.y,
                                  pTxBuffer->imu_ang.z,

                                  pTxBuffer->imu_accel.x,
                                  pTxBuffer->imu_accel.y,
                                  pTxBuffer->imu_accel.z,

                                  pTxBuffer->temp.Temp_front_left,
                                  pTxBuffer->temp.Temp_front_right,
                                  pTxBuffer->temp.Temp_rear_left,
                                  pTxBuffer->temp.Temp_rear_right,

                                  pTxBuffer->gps.longitude,
                                  pTxBuffer->gps.latitude);

            if ((bytewritten == 0) || ret != ESP_OK)
            {
                ESP_LOGE("SDIO", "Error in Writing .CSV File");
                ret = ESP_ERR_NOT_FINISHED;
                return ret; // Failed to write to file
            }
        }
    }

    fclose(f);        // Close the file after writing
    open_file = NULL; // Reset the open file name
    f = NULL;
    return ret;
}

/**================================================================
 * @Fn				- SDIO_SD_Add_Data
 * @breif			- Writes Data in Already created File
 * @param [in]		- file: To select the name and extenstion of File
 * @param [in]		- pTxBuffer: Pointer to buffer storing data to be stored
 * @retval			- Value indicates the States of SD Card (Anything other that ESP_OK is an Error)
 * Note				- For pTxBuffer: .TXT File Types Config -> String only
 * 									 .CSV File Types Config -> f_printf format
 * Warning!			- Function close file to allow for continous Data Storing preiodicly after 7 writes
 * 					- Must close file manually using ((SDIO_SD_Close_file)) after last use
 */
esp_err_t SDIO_SD_Add_Data(SDIO_FileConfig *file, SDIO_TxBuffer *pTxBuffer)
{
    ret = ESP_OK;
    static const char *TAG = "SDIO_SD_Add_Data";

    struct stat st;
    if (stat(file->path, &st) == 0) // Check if the files exists
    {
        //@debug
        // ESP_LOGI(TAG, "Found File Successfully!");
        if (open_file != file->name && f != NULL)
        {
            //@debug
            //ESP_LOGI(TAG, "Closing the File %s now", open_file);
            fclose(f);        // close the previously opened file
            open_file = NULL; // Reset the open file name
            f = NULL;

            f = fopen(file->path, "a");
        }
        else if (open_file == NULL) // There is no file open
        {
            //@debug
            // ESP_LOGI(TAG, "No file is currently Open");
            f = fopen(file->path, "a");
        }

        if (f == NULL)
        {
            //@debug
            ESP_LOGE(TAG, "F is equal to Null | Faild to create File");
            ret = ESP_FAIL; // Failed to open file for writing
        }
        else
        {
            //@debug
            // ESP_LOGI(TAG, "File %s Opened Successfully For appending!", open_file);
            
            open_file = file->name; // Assign the name of the opened file
      
            // Get current Time
            char time_buffer[32];
            if (Time_Sync_get_rtc_time_str(time_buffer, sizeof(time_buffer)) != true)
            {
                ESP_LOGE("RTC", "Failed to get time.");
                strcpy(time_buffer, "XXXXXXXX");
            }

            // Check if file type is .TXT or .CSV
            if (file->type == TXT)
            {
                // Write string to file
                bytewritten = fprintf(f, "%s\n", pTxBuffer->string);
                if ((bytewritten == 0) || ret != ESP_OK)
                {
                    ret = ESP_ERR_NOT_FINISHED;
                    return ret; // Failed to write to file
                }
            }
            else if (file->type == CSV)
            {
                // Write formatted data to file
                bytewritten = fprintf(f, "%s,%s,"
                                         "%u,%u,%u,%u,"
                                         "%u,%u,"
                                         "%u,%u,%u,%u,"
                                         "%u,"
                                         "%u,%u,%u,"
                                         "%u,%u,%u,"
                                         "%u,%u,%u,%u,"
                                         "%f,%f\n",

                                      time_buffer,
                                      pTxBuffer->string,

                                      pTxBuffer->adc.SUS_1,
                                      pTxBuffer->adc.SUS_2,
                                      pTxBuffer->adc.SUS_3,
                                      pTxBuffer->adc.SUS_4,
                                      pTxBuffer->adc.PRESSURE_1,
                                      pTxBuffer->adc.PRESSURE_2,

                                      pTxBuffer->prox_encoder.RPM_front_left,
                                      pTxBuffer->prox_encoder.RPM_front_right,
                                      pTxBuffer->prox_encoder.RPM_rear_left,
                                      pTxBuffer->prox_encoder.RPM_rear_right,
                                      pTxBuffer->prox_encoder.ENCODER_angle,

                                      pTxBuffer->imu_ang.x,
                                      pTxBuffer->imu_ang.y,
                                      pTxBuffer->imu_ang.z,

                                      pTxBuffer->imu_accel.x,
                                      pTxBuffer->imu_accel.y,
                                      pTxBuffer->imu_accel.z,

                                      pTxBuffer->temp.Temp_front_left,
                                      pTxBuffer->temp.Temp_front_right,
                                      pTxBuffer->temp.Temp_rear_left,
                                      pTxBuffer->temp.Temp_rear_right,

                                      pTxBuffer->gps.longitude,
                                      pTxBuffer->gps.latitude);

                if ((bytewritten == 0) || ret != ESP_OK)
                {
                    ret = ESP_ERR_NOT_FINISHED;
                    return ret; // Failed to write to file
                }
            }

            if (writes_Num >= MAX_WRITES)
            {
                fflush(f); // Push buffer to disk
                fclose(f); // Optional but safer after each batch
                open_file = NULL;
                f = NULL;
            }
            else
            {
                writes_Num++;
            }
        }
    }
    else
    {
        ret = ESP_FAIL; // File does not exist
    }
    return ret;
}

/**================================================================
 * @Fn				- SDIO_SD_Read_Data
 * @breif			- Reads Data from an Already created File
 * @param [in]		- file: To select the name and extenstion of File
 * @param [in]		- pRxBuffer: Pointer to buffer storing data to be read
 * @retval			- Value indicates the States of SD Card (Anything other that ESP_OK is an Error)
 * Note				- Used Only for Debuging with UART
 */
esp_err_t SDIO_SD_Read_Data(SDIO_FileConfig *file)
{
    if (open_file != NULL)
    {
        fclose(f);        // close the previously opened file
        open_file = NULL; // Reset the open file name
        f = NULL;
    }
    snprintf(file->path, sizeof(file->path), "%s/%s", MOUNT_POINT, file->name);

    struct stat st;
    if (stat(file->path, &st) == 0) // Check if the files exists
    {
        f = fopen(file->path, "r");

        if (f == NULL)
        {
            ret = ESP_FAIL; // Failed to open file for writing
        }
        else
        {
            open_file = file->name; // Assign the name of the opened file
            char line[MAX_CHAR_SIZE];
            while (fgets(line, sizeof(line), f))
            {
                // Strip newline character if present
                /*
                char *pos = strchr(line, '\n');
                if (pos) *pos = '\0';
                 */
                printf("%s", line);
            }
        }

        fclose(f);        // Close the file after writing
        open_file = NULL; // Reset the open file name
        f = NULL;
    }
    else
    {
        ret = ESP_FAIL; // File does not exist
    }

    return ret;
}

/**================================================================
 * @Fn				- SDIO_SD_Close_file
 * @breif			- Cloeses the currently opened file
 * @param [in]		- Void
 * @retval			- Value indicates the States of SD Card (Anything other that ESP_OK is an Error)
 * Note				- Required to after last use of SDIO_SD_Add_Data
 */
esp_err_t SDIO_SD_Close_file(void)
{
    ret = ESP_OK;
    // Close the file if it is open
    if (fclose(f) == 0)
    {
        open_file = NULL; // Reset the open file name
        f = NULL;
    }
    else
    {
        ret = ESP_FAIL; // Failed to close file
    }
    return ret;
}

/**================================================================
 * @Fn				- SDIO_SD_LOG_CAN_Message
 * @breif			- Creates New .txt File Called SDIO_CAN_txt and last 10 received msgs
 * @param [in]		- rx_msg: pointer to Buffer storing Received Msg
 * @retval			- Value indicates the States of SD Card (Anything other that ESP_OK is an Error)
 * Note				-
 */
esp_err_t SDIO_SD_LOG_CAN_Message(twai_message_t *rx_msg)
{

    // Debug variables
    uint8_t Logged_msgs = 0;
    uint32_t alerts = 0;
    twai_status_info_t s;

    SDIO_FileConfig SDIO_CAN_txt;
    SDIO_TxBuffer buffer;
    static const char *TAG = "SDIO_CAN_DEBUG";
    char time_buffer[32];

    SDIO_CAN_txt.name = "SDIO_CAN.TXT";
    SDIO_CAN_txt.type = TXT;
    buffer.string = "CAN Message Log\r\n";
    if (SDIO_SD_Create_Write_File(&SDIO_CAN_txt, &buffer) == ESP_OK)
    {
        ESP_LOGI(TAG, "SDIO_CAN.txt Created Successfully!");
    }

    char sd_write_buffer[180]; // Adjust size as needed

    // Process rx_msg->identifier, rx_msg->data, etc.

    /*  printf("ID = 0x%03lX\n",rx_msg->identifier);
    printf("Extended? %s\n", rx_msg->extd ? "Yes" : "No");
    printf("RTR? %s\n", rx_msg->rtr ? "Yes" : "No");
    printf("DLC = %d\n", rx_msg->data_length_code);
    for (int i = 0; i < rx_msg->data_length_code; i++) {
        printf("byte[%d] = 0x%02X\n", i, rx_msg->data[i]);
    } */

    for (uint8_t i = 0; i < 10; i++)
    {
        if (twai_receive(rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK)
        {
            if (Time_Sync_get_rtc_time_str(time_buffer, sizeof(time_buffer)) != true)
            {
                ESP_LOGE("RTC", "Failed to get time.");
                strcpy(time_buffer, "XXXXXXXX");
            }

            // Format the message into the string buffer
            snprintf(sd_write_buffer, sizeof(sd_write_buffer),
                     "TimeStamp: %s ID: 0x%03lX Data[0]: 0x%02X Data[1]: 0x%02X Data[2]: 0x%02X Data[3]: 0x%02X "
                     "Data[4]: 0x%02X Data[5]: 0x%02X Data[6]: 0x%02X Data[7]: 0x%02X\r",
                     time_buffer, rx_msg->identifier,
                     rx_msg->data[0], rx_msg->data[1], rx_msg->data[2], rx_msg->data[3],
                     rx_msg->data[4], rx_msg->data[5], rx_msg->data[6], rx_msg->data[7]);

            // Now write this to your SDIO buffer
            buffer.string = sd_write_buffer;

            if (SDIO_SD_Add_Data(&SDIO_CAN_txt, &buffer) == ESP_OK)
            {
                Logged_msgs++;
            }
        }
        else
        {
            ESP_LOGI("CAN", "No message received within the timeout period");
            ret = twai_read_alerts(&alerts, 0);
            if (ret == ESP_OK)
            {
                ESP_LOGI("CAN", "TWAI alert: %08ld", alerts);
            }
            twai_get_status_info(&s);
            ESP_LOGI("CAN", "RX errors: %ld, bus errors: %ld, RX queue full: %ld",
                     s.rx_error_counter, s.bus_error_count, s.rx_missed_count);

            return ESP_FAIL; // No message received
        }
    }

    if (SDIO_SD_Close_file() == ESP_OK)
    {
        ESP_LOGI(TAG, "File Closed Successfully! ... Messages Logged: %u", Logged_msgs);
    }

    SDIO_SD_Read_Data(&SDIO_CAN_txt);
    vTaskDelay(pdMS_TO_TICKS(1000));

    return ESP_OK;
}

/**================================================================
 * @Fn				- compare_file_time_days
 * @breif			- Compares between file last modified time and current time
 *                  - and outputs difference in Days
 * @param [in]		- path: pointer to path of File to be compared
 * @retval			- Value indicates number of days between current date and Last modified date
 * Note				-
 */
uint16_t compare_file_time_days(const char *path)
{

    struct stat st;
    if (stat(path, &st) != 0)
    {
        printf("Failed to stat file: %s\n", path);
        return 0xFFFF; // or another error code
    }
    time_t mod_time = st.st_mtime;
    time_t now = time(NULL);

    // Calculate time difference in seconds
    uint64_t diff_seconds = difftime(now, mod_time);
    // Convert to days
    uint16_t diff_days = (int)(diff_seconds / (60 * 60 * 24));
    ESP_LOGI("Time Diff", "Diff in days %u", diff_days);
    return (diff_days);
}

/**================================================================
 * @Fn				- SDIO_SD_log_can_message_to_csv
 * @breif			- Appends Data to a .CSV File Called SDIO_CAN.CSV and last CAN received msgs
 * @param [in]		- msg: pointer to Buffer storing Received Msg
 * @retval			- Value indicates the States of SD Card (Anything other that ESP_OK is an Error)
 * Note				- The file must be created before calling this function
 */
esp_err_t SDIO_SD_log_can_message_to_csv(twai_message_t *msg)
{
    static const char *TAG = "CAN_LOG";
    SDIO_FileConfig SDIO_CAN_CSV;

    SDIO_CAN_CSV.name = "SDIO_CAN.CSV";
    SDIO_CAN_CSV.type = CSV;
    // Mount point
    snprintf(SDIO_CAN_CSV.path, sizeof(SDIO_CAN_CSV.path), "%s/%s", mount_point, SDIO_CAN_CSV.name);

    FILE *f = fopen(SDIO_CAN_CSV.path, "a"); // Open for appending
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", SDIO_CAN_CSV.path);
        return ESP_FAIL;
    }

    // Optional: Write CSV header once
    fprintf(f, "Timestamp,ID,EXTD,RTR,DLC,DATA[0-7]\n");
    char time_buffer[32];

    if (Time_Sync_get_rtc_time_str(time_buffer, sizeof(time_buffer)) != true)
    {
        ESP_LOGE("RTC", "Failed to get time.");
        strcpy(time_buffer, "XXXXXXXX");
    }

    // Format and log the message
    fprintf(f, "%s,0x%03lX,%s,%s,%d",
            time_buffer,
            msg->identifier,
            msg->extd ? "YES" : "NO",
            msg->rtr ? "YES" : "NO",
            msg->data_length_code);

    for (int i = 0; i < msg->data_length_code; i++)
    {
        fprintf(f, ",0x%02X", msg->data[i]);
    }

    fprintf(f, "\n");
    fclose(f);

    ESP_LOGI(TAG, "Logged CAN message to %s", SDIO_CAN_CSV.name);
    return ESP_OK;
}
