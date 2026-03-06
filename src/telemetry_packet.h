/* Stage 6: Stable wire-format for telemetry packets.
 * Both UDP and MQTT senders convert twai_message_t → telemetry_packet_t
 * before transmission, giving the receiver a fixed 20-byte layout with
 * sequence numbers for drop detection. */
#ifndef TELEMETRY_PACKET_H
#define TELEMETRY_PACKET_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint32_t seq;            // rolling sequence number (detect drops on receiver)
    uint32_t timestamp_ms;   // milliseconds since boot (esp_timer_get_time / 1000)
    uint16_t can_id;         // CAN identifier
    uint8_t  dlc;            // data length code (0-8)
    uint8_t  flags;          // bit 0 = extd, bit 1 = rtr
    uint8_t  data[8];        // raw CAN payload
} telemetry_packet_t;        // 20 bytes total (4+4+2+1+1+8), stable layout

#endif // TELEMETRY_PACKET_H
