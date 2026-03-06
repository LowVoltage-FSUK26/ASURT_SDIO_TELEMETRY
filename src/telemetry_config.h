#ifndef TELEMETRY_CONFIG_H
#define TELEMETRY_CONFIG_H

#define SERVER_IP "192.168.1.14"
#define SERVER_PORT 19132

#define USE_MQTT 1

#if USE_MQTT
#define MQTT_URI       "mqtts://5aeaff002e7c423299c2d92361292d54.s1.eu.hivemq.cloud:8883"
#define MQTT_USER      "yousef"
#define MQTT_PASS      "Yousef123"
#define MQTT_PUB_TOPIC "com/yousef/esp32/data"
extern const char mqtt_root_ca_pem[];
#endif

#define CONNECTIVITY_TEST_IP "8.8.8.8"
#define CONNECTIVITY_TEST_PORT 53
#define CONNECTIVITY_CHECK_INTERVAL_MS 1000
#define CONNECTIVITY_FAIL_THRESHOLD 3

#endif // TELEMETRY_CONFIG_H
