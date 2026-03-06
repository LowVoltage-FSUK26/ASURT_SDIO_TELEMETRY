/*
 * Logging.h
 *
 *  Created on: Jun 22, 2025
 *      Author: Mina Fathy
 */
#ifndef LOGGING_H
#define LOGGING_H

//==================================Standard Libraries Includes=======================//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

//==================================ESP32 Libraries Includes==========================//
#include <driver/gpio.h>
#include <esp_log.h>

#include <driver/sdmmc_host.h>
#include "sdmmc_cmd.h"

#include "driver/twai.h"

//==================================Status Libraries Includes==========================//
#include <sys/unistd.h>
#include <sys/stat.h>

//===================================FATFS Includes===================================//
#include "esp_vfs_fat.h"

//----------------------------
// SDIO Macros
//----------------------------

/* Stage 4: removed ESP_WROOM_32 / ESP32_S3 pin macros (CMD, CLK, D0–D3).
 * They were unused — actual pin config is in logging.c via GPIO_NUM_xx.
 * Pin definitions will move to telemetry_config.h in Stage 8. */

#define SDMMC_BUS_WIDTH_4

#define EMPTY_SDIO_BUFFER(...) 	__VA_ARGS__.string = "Empty Read";\
								__VA_ARGS__.timestamp = "XXXX-XX-XX XX:XX:XX";\
								__VA_ARGS__.adc.SUS_1 = 0;\
								__VA_ARGS__.adc.SUS_2 = 0;\
								__VA_ARGS__.adc.SUS_3 = 0;\
								__VA_ARGS__.adc.SUS_4 = 0;\
								__VA_ARGS__.adc.PRESSURE_1 = 0;\
								__VA_ARGS__.adc.PRESSURE_2 = 0;\
								__VA_ARGS__.prox_encoder.RPM_front_left = 0;\
								__VA_ARGS__.prox_encoder.RPM_front_right = 0;\
								__VA_ARGS__.prox_encoder.RPM_rear_left = 0;\
								__VA_ARGS__.prox_encoder.RPM_rear_right = 0;\
								__VA_ARGS__.prox_encoder.ENCODER_angle = 0;\
								__VA_ARGS__.imu_accel.x = 0;\
								__VA_ARGS__.imu_accel.y = 0;\
								__VA_ARGS__.imu_accel.z = 0;\
								__VA_ARGS__.temp.Temp_front_left = 0;  /* Stage 4: zero temp fields */\
								__VA_ARGS__.temp.Temp_front_right = 0; /* Stage 4: zero temp fields */\
								__VA_ARGS__.temp.Temp_rear_left = 0;   /* Stage 4: zero temp fields */\
								__VA_ARGS__.temp.Temp_rear_right = 0;  /* Stage 4: zero temp fields */\
								__VA_ARGS__.gps.longitude = 0.0f;      /* Stage 4: zero GPS fields */\
								__VA_ARGS__.gps.latitude = 0.0f;       /* Stage 4: zero GPS fields */


/* Stage 4: increased from MAX_CHAR_SIZE 64 — CSV rows can exceed 64 chars */
#define MAX_LINE_SIZE 256
#define MOUNT_POINT "/sdcard"
#define MAX_WRITES 	5
#define EXAMPLE_IS_UHS1 (CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_SDR50 || CONFIG_EXAMPLE_SDMMC_SPEED_UHS_I_DDR50)

#define MAX_DAYS_MODIFIED 2

//----------------------------
// CAN Macros
//----------------------------
#define COMM_CAN_ID_COUNT 6
#define COMM_CAN_ID_FISRT COMM_CAN_ID_IMU_ANGLE

//===============================================
// User type definitions (structures)
//===============================================

//----------------------------
// Sensor Readings Structures
//----------------------------

/* Warning!!! In case adding more IDs to be logged, there is parameters that needed to be updated!

* 1) (Logging.h) COMM_CAN_ID_COUNT: Should be Updated to total No. of IDs
* 2) (Logging.h) Add new struct for decoding received ID
* 3) (logging.h) SDIO_TxBuffer: Add new element for New Sturct
* 4) (Logging.c) Format in .CSV file @ SDIO_SD_Add_Data & SDIO_SD_Add_Data SDIO_SD_Create_Write_File
* 6) (main.c)	 Switch case ID filter @ SDIO_Log_Task_init: Similar Case should be added for New ID

*/
typedef enum	
{
	COMM_CAN_ID_IMU_ANGLE = 0x071,
	COMM_CAN_ID_IMU_ACCEL = 0x072,
	COMM_CAN_ID_ADC = 0x073,
	COMM_CAN_ID_PROX_ENCODER = 0x074,
	COMM_CAN_ID_GPS_LATLONG = 0x075,
	COMM_CAN_ID_TEMP = 0x076,
} COMM_CAN_ID_t;

// ADC Readings Structures
typedef struct
{
	uint64_t SUS_1 : 10;
	uint64_t SUS_2 : 10;
	uint64_t SUS_3 : 10;
	uint64_t SUS_4 : 10;

	uint64_t PRESSURE_1 : 10;
	uint64_t PRESSURE_2 : 10;

} COMM_message_ADC_t;

//----------------------------

// Proximity Encoder Readings Structures
typedef struct
{
	uint64_t RPM_front_left : 11;
	uint64_t RPM_front_right : 11;
	uint64_t RPM_rear_left : 11;
	uint64_t RPM_rear_right : 11;

	uint64_t ENCODER_angle : 10;
	uint64_t Speedkmh : 8;

} COMM_message_PROX_encoder_t;

//----------------------------

// IMU Readings Structures
typedef struct
{
	uint16_t x; /* X-axis Value */
	uint16_t y; /* Y-axis Value */
	uint16_t z; /* Z-axis Value */
} COMM_message_IMU_t;

typedef struct
{

	uint16_t Temp_front_left;
	uint16_t Temp_front_right;
	uint16_t Temp_rear_left;
	uint16_t Temp_rear_right;

} COMM_message_Temp_t;

typedef struct
{
	float longitude;
	float latitude;
} COMM_message_GPS_t;

//----------------------------
// SDIO File Configuration Structures
//----------------------------
typedef struct
{
	char *name; // Specifies the File name to be configured.

	char path[50]; // Specifies the path where the file will be created.

	uint8_t type; // Specifies the file type to be configured.
				  // This parameter must be based on @ref SDIO_File_Types

} SDIO_FileConfig;

//----------------------------
// SD Communication Buffer Structures
typedef struct
{
	const char *string; // String to be Stored in File
						// Used mostly in .TXT files and First column in .CSV files

	const char *timestamp; // Timestamp of the Readings to be stored

	COMM_message_ADC_t adc; // ADC Readings to be stored
							// Used mostly in .CSV files

	COMM_message_PROX_encoder_t prox_encoder; // Proximity Encoder Readings to be stored
											  // Used mostly in .CSV files

	COMM_message_IMU_t imu_ang; // IMU Angle Readings to be stored
								// Used mostly in .CSV files

	COMM_message_IMU_t imu_accel; // IMU Acceleration Readings to be stored
								  // Used mostly in .CSV files

	COMM_message_Temp_t temp; // Temperature Readings to be stored
							  // Used mostly in .CSV files

	COMM_message_GPS_t gps; // GPS Readings to be stored
							// Used mostly in .CSV files

} SDIO_TxBuffer;

//----------------------------
// Macros Configuration References
//---------------------------

//@ref SDIO_File_Types
#define CSV 0
#define TXT 1

//===============================================
// APIs Supported by "LOGGING DRIVER"
//===============================================

esp_err_t SDIO_SD_Init(void);
esp_err_t SDIO_SD_DeInit(void);
esp_err_t SDIO_SD_Create_Write_File(SDIO_FileConfig *file, SDIO_TxBuffer *pTxBuffer);
esp_err_t SDIO_SD_Add_Data(SDIO_FileConfig *file, SDIO_TxBuffer *pTxBuffer);
#if CONFIG_SDIO_DEBUG_READ /* Stage 4: guarded behind debug flag */
esp_err_t SDIO_SD_Read_Data(SDIO_FileConfig *file);
#endif
esp_err_t SDIO_SD_Close_file(void);
/* Stage 4: SDIO_SD_LOG_CAN_Message removed — layering violation (called twai_receive) */
esp_err_t SDIO_SD_log_can_message_to_csv(twai_message_t *msg);
uint16_t compare_file_time_days(const char *path);

#endif