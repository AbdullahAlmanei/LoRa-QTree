# LoRa Mesh Protocol

A minimal, working LoRa mesh protocol. Nodes passively join, pick parents by RSSI + hop distance, and are polled by a gateway that maintains liveness, duty‑cycle aware TX, and small send queues. The same codebase builds as a **gateway** or a **node** via a compile-time flag.

---

## One‑click flashing (web)

Use the browser flasher to program supported boards without installing toolchains:

**Web flasher:** https://abdullahalmanei.github.io/LoRa-QTree


---

## What’s included

- Gateway‑polled mesh: gateway sends QUERY; nodes answer with STATE. Reduces collisions and matches the cited architecture.
- Parent selection: nodes choose parents by recent RSSI and hop distance.
- Robust joining:
  - Node sets its parent only after it actually receives `JOIN_ACK`.
  - Gateway queues and retries `JOIN_ACK` when transmission is deferred; a child is only “activated” after the ACK is truly sent.
- Liveness & misses: the gateway opens a “miss window” only when a QUERY is actually transmitted; any post‑QUERY message from the node resets the miss streak.
- Duty‑cycle aware TX: lenient 1%/hour token‑bucket with borrowing and tiny TX queues so deferred packets (JOIN_ACK, QUERY, STATE, DATA_ACK) eventually go out.
- Optional test traffic: periodic, structured test frames for PDR/hops measurements (`ENABLE_TEST_TX=1`).

---

## Build and flash (PlatformIO)

You can use VS Code + PlatformIO or PlatformIO CLI.

### Requirements

- PlatformIO Core (`pip install platformio`) or VS Code + PlatformIO extension
- USB drivers for your board

### Example environments

Node (T‑Beam S3 example):

```ini
[env:tbeam-s3-node]
platform = espressif32
board = tbeam_supreme
framework = arduino
monitor_speed = 115200
upload_speed  = 921600
lib_deps =
    jgromes/RadioLib@^7.2.1
    olikraus/U8g2@^2.34.24
    lewisxhe/XPowersLib@^0.2.6
build_flags =
    -D TBEAM_S3_NODE
    -D ROLE_NODE
    -D CORE_DEBUG_LEVEL=5
    ; enable periodic test frames (optional)
    -D ENABLE_TEST_TX=1
```

Gateway:

```ini
[env:esp32-gw]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    jgromes/RadioLib@^7.2.1
    olikraus/U8g2@^2.34.24
build_flags =
    -D ROLE_GATEWAY
    -D CORE_DEBUG_LEVEL=5
```

### Build, upload, and monitor

```bash
# build
pio run -e tbeam-s3-node
pio run -e esp32-gw

# flash
pio run -e tbeam-s3-node -t upload
pio run -e esp32-gw -t upload

# serial monitor
pio device monitor -b 115200
```

---

## Quick start

1. Flash the **gateway** and power it up.
2. Flash one or more **nodes**. Place them across rooms/floors.
3. The gateway periodically sends **QUERY** to known nodes. Nodes **STATE** back.
4. A node picks a parent by RSSI + hop distance, sends `JOIN_REQ`, and only sets the parent after receiving `JOIN_ACK`.
5. The gateway uses queues for JOIN_ACK and QUERY so deferrals are retried automatically.

---

## Build‑time flags

| Flag | Purpose |
|------|---------|
| `ROLE_NODE` / `ROLE_GATEWAY` | Compile as node or gateway (mutually exclusive). |
| `TBEAM_S3_NODE`, `HELTEC_V3_NODE` | Board helpers for PMU/battery; harmless if unsupported (battery falls back to 0 mV). |
| `ENABLE_TEST_TX=1` | Node emits a structured test frame about every 90 s. |
| `CORE_DEBUG_LEVEL=5` | Verbose logs. Reduce for quieter output. |
| `MAX_HOPS` | Hop cap (default 3). |

Radio settings (frequency/BW/SF/CR/sync word) must match across all devices. The project uses RadioLib; set modulation during your board init. Example used during development: 868 MHz, BW 125 kHz, SF12, CR 4/5, sync 0x12.

---

## Using the protocol

- Application data: send as `DATA_UP`; the gateway replies with `DATA_ACK`.
- Topology changes: nodes inform the gateway when they add/remove children; the gateway maintains a global table.
- Test features: enable `ENABLE_TEST_TX=1` to collect structured telemetry for PDR/hops comparison between protocol variants or PHY settings.

---

## Reference

Lee, H.-C., & Ke, K.-H. (2018). Monitoring of Large‑Area IoT Sensors Using a LoRa Wireless Mesh Network System: Design and Evaluation. IEEE Transactions on Instrumentation and Measurement. https://doi.org/10.1109/TIM.2018.2814082

The optional test features in this repo are designed to support scientific comparisons across mesh variants and parameter sets.

---

## Acknowledgements

This was created during the ICTP TRIL FEllowship and the KAUST ACADEMY, supervised by DR. Moez Altayeb, Dr Marco Zennaro, Salah Abdeljabar.

---

## License

MIT. See [LICENSE](LICENSE).
