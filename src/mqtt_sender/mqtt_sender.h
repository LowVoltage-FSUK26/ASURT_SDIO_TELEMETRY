#ifndef MQTT_SENDER_H
#define MQTT_SENDER_H

#include "mqtt_client.h"

void mqtt_sender_task(void *pvParameters);

/* Stage 9: Shared MQTT client handle for heartbeat task to publish on */
extern esp_mqtt_client_handle_t mqtt_heartbeat_client;

#endif // MQTT_SENDER_H
