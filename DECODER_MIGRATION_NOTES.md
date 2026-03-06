# Stage 6 — Decoder Migration Notes

## What changed on the ESP32 side

Both `udp_sender` and `mqtt_sender` no longer send a raw `twai_message_t`.
They now send a **packed 24-byte `telemetry_packet_t`** defined in
`src/telemetry_packet.h`:

```c
typedef struct __attribute__((packed)) {
    uint32_t seq;            // rolling sequence number
    uint32_t timestamp_ms;   // ms since ESP32 boot
    uint16_t can_id;         // CAN arbitration ID
    uint8_t  dlc;            // data length code (0-8)
    uint8_t  flags;          // bit 0 = extended frame, bit 1 = RTR
    uint8_t  data[8];        // raw CAN payload
} telemetry_packet_t;        // 24 bytes, little-endian
```

## Wire layout (byte map)

```
Offset  Size  Type       Field
──────  ────  ─────────  ─────────────
 0      4     uint32_t   seq
 4      4     uint32_t   timestamp_ms
 8      2     uint16_t   can_id
10      1     uint8_t    dlc
11      1     uint8_t    flags
12      8     uint8_t[]  data[8]
──────
24 bytes total
```

All multi-byte fields are **little-endian** (ESP32 native byte order).

---

## How to update the Qt / C++ GUI

### 1. Define the matching struct

Add this to a header in your GUI project (e.g. `telemetry_packet.h`):

```cpp
#pragma once
#include <cstdint>

#pragma pack(push, 1)
struct TelemetryPacket {
    uint32_t seq;
    uint32_t timestamp_ms;
    uint16_t can_id;
    uint8_t  dlc;
    uint8_t  flags;
    uint8_t  data[8];
};
#pragma pack(pop)

static_assert(sizeof(TelemetryPacket) == 24, "Packet must be 24 bytes");
```

### 2. Update the UDP receive path

Wherever you currently read from the UDP socket and interpret the bytes as
`twai_message_t`, replace with:

```cpp
// Old code (example):
// QByteArray datagram = socket->receiveDatagram().data();
// twai_message_t msg;
// memcpy(&msg, datagram.constData(), sizeof(msg));

// New code:
QByteArray datagram = socket->receiveDatagram().data();
if (datagram.size() < static_cast<int>(sizeof(TelemetryPacket))) {
    qWarning() << "Runt packet, ignoring (" << datagram.size() << "bytes)";
    continue; // or return
}

TelemetryPacket pkt;
memcpy(&pkt, datagram.constData(), sizeof(TelemetryPacket));

// If your desktop is big-endian (very unlikely on x86), byte-swap:
// pkt.seq          = qFromLittleEndian(pkt.seq);
// pkt.timestamp_ms = qFromLittleEndian(pkt.timestamp_ms);
// pkt.can_id       = qFromLittleEndian(pkt.can_id);
// On x86/x64 this is a no-op so you can skip it.
```

### 3. Update the MQTT receive path

Same struct, just pulled from the MQTT message payload:

```cpp
void onMqttMessage(const QByteArray &payload) {
    if (payload.size() < static_cast<int>(sizeof(TelemetryPacket))) {
        qWarning() << "MQTT payload too small";
        return;
    }
    TelemetryPacket pkt;
    memcpy(&pkt, payload.constData(), sizeof(TelemetryPacket));
    // ... process pkt
}
```

### 4. Extract CAN fields from the new struct

```cpp
// CAN ID
uint16_t canId = pkt.can_id;

// Extended / RTR flags
bool isExtended = pkt.flags & 0x01;
bool isRTR      = pkt.flags & 0x02;

// Payload (only first `dlc` bytes are meaningful)
uint8_t payload[8];
memcpy(payload, pkt.data, pkt.dlc);
```

### 5. Use the sequence number for drop detection

```cpp
static uint32_t lastSeq = 0;
static bool     firstPacket = true;

if (firstPacket) {
    firstPacket = false;
} else {
    uint32_t expected = lastSeq + 1;
    if (pkt.seq != expected) {
        uint32_t dropped = pkt.seq - expected; // handles wrap-around for small gaps
        qWarning() << "Dropped" << dropped << "packet(s) — seq jumped from"
                   << lastSeq << "to" << pkt.seq;
    }
}
lastSeq = pkt.seq;
```

### 6. Use timestamp_ms for time-series plots

`timestamp_ms` is milliseconds since the ESP32 booted. Use it as the X-axis
for any time-series chart instead of (or alongside) local receive time:

```cpp
double seconds = pkt.timestamp_ms / 1000.0;
chart->appendPoint(seconds, someValue);
```

---

## How to do it in Python (if you have a Python receiver too)

### Decode a single 24-byte datagram

```python
import struct

PACKET_FORMAT = "<IIHBB8s"   # little-endian: u32 u32 u16 u8 u8 8-bytes
PACKET_SIZE   = 24

def decode_packet(buf: bytes) -> dict:
    """Decode a 24-byte telemetry_packet_t from the ESP32."""
    if len(buf) < PACKET_SIZE:
        raise ValueError(f"Expected {PACKET_SIZE} bytes, got {len(buf)}")

    seq, ts_ms, can_id, dlc, flags, raw_data = struct.unpack_from(PACKET_FORMAT, buf, 0)

    return {
        "seq":          seq,
        "timestamp_ms": ts_ms,
        "can_id":       can_id,
        "dlc":          dlc,
        "extended":     bool(flags & 0x01),
        "rtr":          bool(flags & 0x02),
        "data":         raw_data[:dlc],      # only dlc bytes are valid
    }
```

### UDP listener example

```python
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 19132))

last_seq = None
while True:
    data, addr = sock.recvfrom(64)
    pkt = decode_packet(data)

    # Drop detection
    if last_seq is not None and pkt["seq"] != last_seq + 1:
        dropped = pkt["seq"] - (last_seq + 1)
        print(f"⚠ Dropped {dropped} packet(s)  (seq {last_seq} → {pkt['seq']})")
    last_seq = pkt["seq"]

    print(f"seq={pkt['seq']:>6}  ts={pkt['timestamp_ms']:>10}ms  "
          f"CAN 0x{pkt['can_id']:03X}  DLC={pkt['dlc']}  "
          f"data={pkt['data'].hex(' ')}")
```

### MQTT listener example (paho-mqtt)

```python
import paho.mqtt.client as mqtt

def on_message(client, userdata, msg):
    pkt = decode_packet(msg.payload)
    print(pkt)

client = mqtt.Client()
client.tls_set()  # if using mqtts
client.username_pw_set("user", "pass")
client.on_message = on_message
client.connect("your-broker.hivemq.cloud", 8883)
client.subscribe("com/yousef/esp32/data")
client.loop_forever()
```

---

## Quick checklist

- [ ] Add `TelemetryPacket` struct (or Python `struct.unpack`) to the GUI
- [ ] Replace old `twai_message_t` decoding with the new 24-byte layout
- [ ] Add sequence-number gap detection (log or display drops)
- [ ] Use `timestamp_ms` for time-axis on plots
- [ ] Handle the `flags` byte for extended/RTR instead of old bitfield access
- [ ] Test with both UDP and MQTT — they now produce identical payloads
