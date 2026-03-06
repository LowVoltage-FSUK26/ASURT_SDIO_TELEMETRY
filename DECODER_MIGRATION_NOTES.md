# ASURT Telemetry — Full Decoding Guide

## 1. Wire Packet Format (`telemetry_packet_t`)

Every message arriving over MQTT or UDP is a **20-byte binary packet**.
All multi-byte fields are **little-endian** (ESP32 native byte order).

```
Byte offset   Size   C type      Field            Notes
───────────   ────   ─────────   ───────────────  ──────────────────────────────────────
 0 – 3         4     uint32_t    seq              Rolling counter. Increments by 1 per
                                                  frame sent. Use to detect dropped packets:
                                                  gap = current_seq − (last_seq + 1).
 4 – 7         4     uint32_t    timestamp_ms     Milliseconds since ESP32 boot
                                                  (esp_timer_get_time() / 1000).
                                                  Use as the X-axis for all time-series plots.
 8 – 9         2     uint16_t    can_id           CAN arbitration ID (see Section 2).
10             1     uint8_t     dlc              Data Length Code — number of valid bytes
                                                  in data[8] (always 8 for this system).
11             1     uint8_t     flags            bit 0 = extended frame (always 0 here)
                                                  bit 1 = RTR frame     (always 0 here)
12 – 19        8     uint8_t[8]  data             Raw CAN payload. Decode according to can_id
                                                  (see Section 2).
───────────────────────────────────────────────────────────────────────────────────────────
TOTAL: 20 bytes
```

### 1.1 Python unpack

```python
import struct

PACKET_FORMAT = "<IIHBB8s"                    # exactly matches telemetry_packet_t
PACKET_SIZE   = struct.calcsize(PACKET_FORMAT) # 20 bytes

def decode_envelope(buf: bytes) -> dict:
    if len(buf) < PACKET_SIZE:
        raise ValueError(f"Short packet: {len(buf)} bytes (need {PACKET_SIZE})")
    seq, ts_ms, can_id, dlc, flags, data = struct.unpack_from(PACKET_FORMAT, buf)
    return {
        "seq":          seq,
        "timestamp_ms": ts_ms,
        "timestamp_s":  ts_ms / 1000.0,
        "can_id":       can_id,
        "dlc":          dlc,
        "extended":     bool(flags & 0x01),
        "rtr":          bool(flags & 0x02),
        "data":         data[:dlc],   # only first `dlc` bytes are valid CAN payload
        "data_raw":     data,         # full 8-byte field (needed by bit-mask decoders)
    }
```

### 1.2 Drop detection

```python
last_seq = None

def check_drops(pkt: dict) -> int:
    global last_seq
    drops = 0
    if last_seq is not None:
        expected = (last_seq + 1) & 0xFFFFFFFF   # handles uint32 wrap-around
        if pkt["seq"] != expected:
            drops = (pkt["seq"] - expected) & 0xFFFFFFFF
            print(f"⚠  {drops} packet(s) dropped  "
                  f"(seq jumped {last_seq} → {pkt['seq']})")
    last_seq = pkt["seq"]
    return drops
```

---

## 2. CAN ID Reference

| CAN ID  | Name              | Payload struct              | Section |
|---------|-------------------|-----------------------------|---------|
| `0x071` | IMU Angle         | `COMM_message_IMU_t`        | 2.1     |
| `0x072` | IMU Acceleration  | `COMM_message_IMU_t`        | 2.2     |
| `0x073` | ADC               | `COMM_message_ADC_t`        | 2.3     |
| `0x074` | Prox / Encoder    | `COMM_message_PROX_encoder_t` | 2.4  |
| `0x075` | GPS               | `COMM_message_GPS_t`        | 2.5     |
| `0x076` | Temperature       | `COMM_message_Temp_t`       | 2.6     |

---

### 2.1  IMU Angle — `0x071`

**C struct:**
```c
typedef struct {
    uint16_t x;   // X-axis angle
    uint16_t y;   // Y-axis angle
    uint16_t z;   // Z-axis angle
} COMM_message_IMU_t;
```

**Wire layout inside `data[8]`:**
```
Byte  0–1   uint16_t   x   (little-endian signed int16)
Byte  2–3   uint16_t   y   (little-endian signed int16)
Byte  4–5   uint16_t   z   (little-endian signed int16)
Byte  6–7   (unused)
```

**Note:** The C type is `uint16_t` but the values are transmitted as signed angles.
Interpret as `int16_t` (range −32768 to +32767). Expected physical range: X/Z ±180°, Y ±90°.

**Python decoder:**
```python
def decode_imu_angle(data: bytes) -> dict:
    x, y, z = struct.unpack_from("<hhh", data)   # h = signed int16
    return {"IMU_Ang_X": x, "IMU_Ang_Y": y, "IMU_Ang_Z": z}
```

---

### 2.2  IMU Acceleration — `0x072`

Same struct and wire layout as IMU Angle. Values represent acceleration in each axis.
Expected physical range: 0–16 g (raw units, no scaling applied in firmware).

**Python decoder:**
```python
def decode_imu_accel(data: bytes) -> dict:
    x, y, z = struct.unpack_from("<hhh", data)
    return {"IMU_Accel_X": x, "IMU_Accel_Y": y, "IMU_Accel_Z": z}
```

---

### 2.3  ADC — `0x073`

**C struct:**
```c
typedef struct {
    uint64_t SUS_1     : 10;   // Suspension potentiometer 1
    uint64_t SUS_2     : 10;   // Suspension potentiometer 2
    uint64_t SUS_3     : 10;   // Suspension potentiometer 3
    uint64_t SUS_4     : 10;   // Suspension potentiometer 4
    uint64_t PRESSURE_1: 10;   // Pressure sensor 1
    uint64_t PRESSURE_2: 10;   // Pressure sensor 2
} COMM_message_ADC_t;
```

Six 10-bit values tightly packed into a single `uint64_t` (8 bytes).
Total bits used: 60. Bits 60–63 are always 0.

**Wire layout inside `data[8]`:**
```
Bits  0 –  9   SUS_1       raw ADC (0–1023)
Bits 10 – 19   SUS_2       raw ADC (0–1023)
Bits 20 – 29   SUS_3       raw ADC (0–1023)
Bits 30 – 39   SUS_4       raw ADC (0–1023)
Bits 40 – 49   PRESSURE_1  raw ADC (0–1023)
Bits 50 – 59   PRESSURE_2  raw ADC (0–1023)
Bits 60 – 63   (unused, always 0)
```

**Python decoder:**
```python
def decode_adc(data: bytes) -> dict:
    raw = struct.unpack_from("<Q", data)[0]   # Q = uint64_t
    return {
        "SUS_1":      (raw >>  0) & 0x3FF,
        "SUS_2":      (raw >> 10) & 0x3FF,
        "SUS_3":      (raw >> 20) & 0x3FF,
        "SUS_4":      (raw >> 30) & 0x3FF,
        "PRESSURE_1": (raw >> 40) & 0x3FF,
        "PRESSURE_2": (raw >> 50) & 0x3FF,
    }
```

---

### 2.4  Proximity / Encoder — `0x074`

**C struct:**
```c
typedef struct {
    uint64_t RPM_front_left  : 11;   // Wheel speed — front left
    uint64_t RPM_front_right : 11;   // Wheel speed — front right
    uint64_t RPM_rear_left   : 11;   // Wheel speed — rear left
    uint64_t RPM_rear_right  : 11;   // Wheel speed — rear right
    uint64_t ENCODER_angle   : 10;   // Steering encoder angle
    uint64_t Speedkmh        :  8;   // Vehicle speed (km/h)
} COMM_message_PROX_encoder_t;
```

**Wire layout inside `data[8]`:**
```
Bits  0 – 10   RPM_front_left   (0–2047 RPM)
Bits 11 – 21   RPM_front_right  (0–2047 RPM)
Bits 22 – 32   RPM_rear_left    (0–2047 RPM)
Bits 33 – 43   RPM_rear_right   (0–2047 RPM)
Bits 44 – 53   ENCODER_angle    (0–1023 raw)
Bits 54 – 61   Speedkmh         (0–255 km/h)
Bits 62 – 63   (unused, always 0)
```

**Python decoder:**
```python
def decode_prox_encoder(data: bytes) -> dict:
    raw = struct.unpack_from("<Q", data)[0]
    return {
        "RPM_FL":    (raw >>  0) & 0x7FF,
        "RPM_FR":    (raw >> 11) & 0x7FF,
        "RPM_RL":    (raw >> 22) & 0x7FF,
        "RPM_RR":    (raw >> 33) & 0x7FF,
        "ENC_ANGLE": (raw >> 44) & 0x3FF,
        "Speedkmh":  (raw >> 54) & 0xFF,
    }
```

---

### 2.5  GPS — `0x075`

**C struct:**
```c
typedef struct {
    float longitude;   // degrees, WGS-84
    float latitude;    // degrees, WGS-84
} COMM_message_GPS_t;
```

**Wire layout inside `data[8]`:**
```
Byte 0–3   float32   longitude   little-endian IEEE 754
Byte 4–7   float32   latitude    little-endian IEEE 754
```

⚠ **Field order:** `longitude` is first in the struct and therefore first on the wire.
This is the opposite of the conventional (lat, lon) convention. The CSV header and
the dashboard both display latitude before longitude for readability, but on the wire
longitude always comes first.

**Python decoder:**
```python
def decode_gps(data: bytes) -> dict:
    lon, lat = struct.unpack_from("<ff", data)
    return {"GPS_Long": lon, "GPS_Lat": lat}
```

---

### 2.6  Temperature — `0x076`

**C struct:**
```c
typedef struct {
    uint16_t Temp_front_left;
    uint16_t Temp_front_right;
    uint16_t Temp_rear_left;
    uint16_t Temp_rear_right;
} COMM_message_Temp_t;
```

**Wire layout inside `data[8]`:**
```
Byte 0–1   uint16_t   Temp_front_left    raw units (0–300 expected range)
Byte 2–3   uint16_t   Temp_front_right
Byte 4–5   uint16_t   Temp_rear_left
Byte 6–7   uint16_t   Temp_rear_right
```

**Python decoder:**
```python
def decode_temp(data: bytes) -> dict:
    fl, fr, rl, rr = struct.unpack_from("<hhhh", data)
    return {"Temp_FL": fl, "Temp_FR": fr, "Temp_RL": rl, "Temp_RR": rr}
```

---

## 3. Full Dispatcher

Drop this block into any receiver script. It decodes the outer packet then
dispatches to the correct CAN decoder based on `can_id`.

```python
import struct

# ── Packet envelope ───────────────────────────────────────────────────────────
PACKET_FORMAT = "<IIHBB8s"
PACKET_SIZE   = struct.calcsize(PACKET_FORMAT)  # 20

# ── CAN IDs ───────────────────────────────────────────────────────────────────
CAN_IMU_ANGLE    = 0x071
CAN_IMU_ACCEL    = 0x072
CAN_ADC          = 0x073
CAN_PROX_ENCODER = 0x074
CAN_GPS          = 0x075
CAN_TEMP         = 0x076


def decode_envelope(buf: bytes) -> dict:
    if len(buf) < PACKET_SIZE:
        raise ValueError(f"Short packet: {len(buf)} B (need {PACKET_SIZE})")
    seq, ts_ms, can_id, dlc, flags, data = struct.unpack_from(PACKET_FORMAT, buf)
    return {
        "seq":          seq,
        "timestamp_ms": ts_ms,
        "timestamp_s":  ts_ms / 1000.0,
        "can_id":       can_id,
        "dlc":          dlc,
        "extended":     bool(flags & 0x01),
        "rtr":          bool(flags & 0x02),
        "data":         data[:dlc],
        "data_raw":     data,
    }


def decode_can_payload(can_id: int, data: bytes) -> dict:
    """Decode the 8-byte CAN payload for the given CAN ID.
    Returns a flat dict of field_name → value.
    Returns an empty dict for unknown IDs."""

    if can_id == CAN_IMU_ANGLE:
        x, y, z = struct.unpack_from("<hhh", data)
        return {"IMU_Ang_X": x, "IMU_Ang_Y": y, "IMU_Ang_Z": z}

    elif can_id == CAN_IMU_ACCEL:
        x, y, z = struct.unpack_from("<hhh", data)
        return {"IMU_Accel_X": x, "IMU_Accel_Y": y, "IMU_Accel_Z": z}

    elif can_id == CAN_ADC:
        raw = struct.unpack_from("<Q", data)[0]
        return {
            "SUS_1":      (raw >>  0) & 0x3FF,
            "SUS_2":      (raw >> 10) & 0x3FF,
            "SUS_3":      (raw >> 20) & 0x3FF,
            "SUS_4":      (raw >> 30) & 0x3FF,
            "PRESSURE_1": (raw >> 40) & 0x3FF,
            "PRESSURE_2": (raw >> 50) & 0x3FF,
        }

    elif can_id == CAN_PROX_ENCODER:
        raw = struct.unpack_from("<Q", data)[0]
        return {
            "RPM_FL":    (raw >>  0) & 0x7FF,
            "RPM_FR":    (raw >> 11) & 0x7FF,
            "RPM_RL":    (raw >> 22) & 0x7FF,
            "RPM_RR":    (raw >> 33) & 0x7FF,
            "ENC_ANGLE": (raw >> 44) & 0x3FF,
            "Speedkmh":  (raw >> 54) & 0xFF,
        }

    elif can_id == CAN_GPS:
        lon, lat = struct.unpack_from("<ff", data)
        return {"GPS_Long": lon, "GPS_Lat": lat}

    elif can_id == CAN_TEMP:
        fl, fr, rl, rr = struct.unpack_from("<hhhh", data)
        return {"Temp_FL": fl, "Temp_FR": fr, "Temp_RL": rl, "Temp_RR": rr}

    return {}   # unknown CAN ID


def decode_packet(buf: bytes) -> dict:
    """Decode the full 20-byte packet.
    Returns the envelope fields merged with the decoded CAN fields."""
    pkt    = decode_envelope(buf)
    fields = decode_can_payload(pkt["can_id"], pkt["data_raw"])
    return {**pkt, "fields": fields}
```

### Usage example

```python
# Single packet
pkt = decode_packet(raw_bytes)
print(f"seq={pkt['seq']}  t={pkt['timestamp_s']:.3f}s  "
      f"ID=0x{pkt['can_id']:03X}  {pkt['fields']}")

# With drop detection
last_seq = None
def process(raw: bytes):
    global last_seq
    pkt = decode_packet(raw)
    if last_seq is not None:
        gap = (pkt["seq"] - last_seq - 1) & 0xFFFFFFFF
        if gap:
            print(f"⚠  {gap} packet(s) dropped before seq={pkt['seq']}")
    last_seq = pkt["seq"]
    return pkt
```

---

## 4. Heartbeat Packet

The heartbeat is published separately on `<data_topic>/heartbeat` as a plain
UTF-8 JSON string (not a binary packet). No length check needed.

**Format:**
```json
{"uptime_s":123,"free_heap":145000,"sd_ok":1,"can_fps":50}
```

| Field       | Type   | Meaning                                           |
|-------------|--------|---------------------------------------------------|
| `uptime_s`  | int    | Seconds since ESP32 boot                          |
| `free_heap` | int    | Free heap bytes — watch for downward drift        |
| `sd_ok`     | int    | `1` = SD card logging running, `0` = failed       |
| `can_fps`   | int    | CAN frames received per second in the last period |

**Python decoder:**
```python
import json

def decode_heartbeat(payload: bytes) -> dict:
    return json.loads(payload.decode("utf-8"))
```

---

## 5. Complete Field Reference

| Field         | CAN ID  | Raw type       | Bit width | Expected range    |
|---------------|---------|----------------|-----------|-------------------|
| IMU_Ang_X     | `0x071` | int16          | 16        | −180 to +180      |
| IMU_Ang_Y     | `0x071` | int16          | 16        | −90 to +90        |
| IMU_Ang_Z     | `0x071` | int16          | 16        | −180 to +180      |
| IMU_Accel_X   | `0x072` | int16          | 16        | 0 to 16 (g)       |
| IMU_Accel_Y   | `0x072` | int16          | 16        | 0 to 16 (g)       |
| IMU_Accel_Z   | `0x072` | int16          | 16        | 0 to 16 (g)       |
| SUS_1         | `0x073` | uint10 bitpack | 10        | 0 to 1023         |
| SUS_2         | `0x073` | uint10 bitpack | 10        | 0 to 1023         |
| SUS_3         | `0x073` | uint10 bitpack | 10        | 0 to 1023         |
| SUS_4         | `0x073` | uint10 bitpack | 10        | 0 to 1023         |
| PRESSURE_1    | `0x073` | uint10 bitpack | 10        | 0 to 1023         |
| PRESSURE_2    | `0x073` | uint10 bitpack | 10        | 0 to 1023         |
| RPM_FL        | `0x074` | uint11 bitpack | 11        | 0 to 2047         |
| RPM_FR        | `0x074` | uint11 bitpack | 11        | 0 to 2047         |
| RPM_RL        | `0x074` | uint11 bitpack | 11        | 0 to 2047         |
| RPM_RR        | `0x074` | uint11 bitpack | 11        | 0 to 2047         |
| ENC_ANGLE     | `0x074` | uint10 bitpack | 10        | 0 to 1023         |
| Speedkmh      | `0x074` | uint8 bitpack  |  8        | 0 to 255          |
| GPS_Long      | `0x075` | float32 (1st)  | 32        | −180.0 to +180.0  |
| GPS_Lat       | `0x075` | float32 (2nd)  | 32        | −90.0 to +90.0    |
| Temp_FL       | `0x076` | int16          | 16        | 0 to 300 (raw)    |
| Temp_FR       | `0x076` | int16          | 16        | 0 to 300 (raw)    |
| Temp_RL       | `0x076` | int16          | 16        | 0 to 300 (raw)    |
| Temp_RR       | `0x076` | int16          | 16        | 0 to 300 (raw)    |

---

## 6. Common Mistakes

**Using offset 4 for can_id:** The old (pre-Stage-6) firmware sent a raw
`twai_message_t` where the 32-bit identifier was at byte offset 4. In the new
format `can_id` is a `uint16_t` at byte offset **8**. The format string
`"<L B 8s"` at offset 4 is wrong — use `"<IIHBB8s"` at offset 0.

**Expecting 24 bytes:** The `telemetry_packet.h` comment says "24 bytes total"
but the struct is actually `4+4+2+1+1+8 = 20 bytes`. `__attribute__((packed))`
changes nothing here because every field is already naturally aligned.

**GPS field order:** The struct has `longitude` first, `latitude` second.
`struct.unpack_from("<ff", data)` returns `(longitude, latitude)` — not
`(latitude, longitude)`.

**Signed vs unsigned for IMU:** The C typedef uses `uint16_t` but values are
transmitted as signed angles. Always unpack with `"<hhh"` (lowercase h =
signed int16), not `"<HHH"` (uppercase = unsigned).

**Interpreting ADC/RPM without masking:** After unpacking the uint64, you
**must** apply the bitmask before using the value. For example, without
`& 0x3FF` for a 10-bit field, adjacent fields bleed into each other.