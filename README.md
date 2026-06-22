# Vandal CTF — Firmware

> **Vulnerable-by-design** multi-protocol security testing platform built on two stacked **ESP32-C6** boards.

The _master_ board transmits intentionally weak payloads across 8 communication protocols.
The _slave_ board receives, decodes, and reports everything back to a web dashboard served by the master.

---

## ✨ Features

| Protocol     | Transport                                  | Vulnerability by Design                        |
| ------------ | ------------------------------------------ | ---------------------------------------------- |
| **UART**     | GPIO16 ↔ GPIO17, 115 200 baud              | Plain-text serial, no framing                  |
| **I²C**      | SDA GPIO22 / SCL GPIO23, 100 kHz           | Unencrypted bus, fixed slave address `0x28`    |
| **SPI**      | MOSI 18 / MISO 20 / SCLK 19 / CS 21, 1 MHz | No authentication on the bus                   |
| **ESP-NOW**  | WiFi broadcast, channel 1                  | Broadcast MAC, no encryption                   |
| **BLE Open** | GATT service `0000aa00-…`                  | Characteristic readable without pairing        |
| **BLE Auth** | GATT service `0000bb00-…`                  | Static 6-digit PIN (`001234`), Legacy Pairing  |
| **Thread**   | IEEE 802.15.4, channel 15, PAN `0xFACE`    | Well-known network key, UDP multicast          |
| **HTTP**     | WiFi TCP port 80                           | Payload in plaintext over open SoftAP          |

### System Monitoring

The master exposes a **`GET /system`** endpoint returning real-time telemetry:

- Heap usage (total / free / min-free)
- Internal temperature (°C)
- Uptime, FreeRTOS task count
- ESP-IDF version
- Per-protocol running state & custom payload

### On-Demand Protocol Control

The master boots with **WiFi AP + HTTP server only**. Each protocol is started
or stopped at runtime through the web dashboard (**`POST /control`**).
Heavy initializations (OpenThread, NimBLE) run in dedicated FreeRTOS tasks
so the HTTP server stays responsive.

### Custom Payloads

Each protocol's payload can be overridden from the dashboard
(**`POST /payload`**). Clear the value to revert to the auto-generated default.

---

## 🎯 Vandal Capabilities

Each CTF service maps to a specific capability in the [Vandal](../Vandal/) security
analysis platform. The table below shows which Vandal command or module targets
each service.

| CTF Service  | Vandal Module / Command                       | What to Demonstrate                                   |
| ------------ | --------------------------------------------- | ----------------------------------------------------- |
| **UART**     | Passive serial capture / protocol analysis    | Plain-text payload recovery                           |
| **I²C**      | I²C bus monitor / logic analyser              | Unencrypted sensor data capture                       |
| **SPI**      | SPI bus capture                               | No-auth bus snooping                                  |
| **ESP-NOW**  | `esp_now` sniffer (WiFi monitor mode)         | Broadcast frame capture, no encryption                |
| **BLE Open** | `bt_scanner` (GATT enumeration)               | Discover open characteristic, read flag without pairing |
| **BLE Auth** | `bt_probe_start` (PIN dictionary attack)      | Identify `PASSKEY_REQUIRED` class, crack PIN `001234` via dictionary, read protected characteristic |
| **Thread**   | Roadmap — OpenThread network analysis         | Capture 802.15.4 frames, known network key extraction |
| **HTTP**     | `wifi_sniffer` (probe capture) + HTTP sniff   | Intercept HTTP payload on open WiFi                   |

### BLE Auth — Vandal workflow

```
bt_probe_start targets=[<slave_mac>] pins=["0000","1234","001234"]
  → preflight: BT_AUTH_CLASS_PASSKEY_REQUIRED
  → dictionary attempt "001234" → pairing_success: true
  → post-pairing: bt_write or GATT read on service 0000bb00-…
```

---

## 🏗️ Architecture

```
┌─────────────────── MASTER ────────────────────────┐
│  WiFi AP ("Vandal CTF")                           │
│  HTTP Server (port 80)                            │
│    ├─ GET  /            → React SPA (FATFS)       │
│    ├─ GET  /status      → merged payload report   │
│    ├─ GET  /system      → system telemetry        │
│    ├─ POST /control     → start/stop protocol     │
│    ├─ POST /payload     → custom payload          │
│    ├─ POST /messages    → slave JSON report       │
│    └─ POST /http-payload→ HTTP service endpoint   │
│                                                   │
│  Protocol TX (on demand, 8 services)              │
│  UART · I²C · SPI · ESP-NOW · BLE · Thread · HTTP │
└──────────────┬────────────────────────────────────┘
        wired + wireless
┌──────────────┴────────────────────────────────────┐
│                   SLAVE                           │
│  All RX modules started at boot                  │
│  Event bus → HTTP Client POST to master          │
└───────────────────────────────────────────────────┘
```

### Data Flow

```
Master TX event
  → esp_event_post(VANDAL_EVT_PAYLOAD_RECEIVED)
  → master_payload_event_handler() updates s_master_payloads[]
  → GET /status returns merged view (master + slave data)

Slave RX event
  → http_event_handler() stores in s_payloads[]
  → POST /messages every 5 s to master
  → messages_post_handler() updates s_last_report
  → GET /status merges s_last_report with s_master_payloads[]
```

### Component Map

```
components/
├── vandal_common/   # Types, event bus, payload generator, running-state API
├── vandal_wifi/     # SoftAP (master) + STA (slave) setup
├── vandal_http/     # HTTP server (master) / HTTP client (slave)
├── vandal_uart/     # UART1 master TX / slave RX
├── vandal_i2c/      # I²C master write / slave v2 driver
├── vandal_spi/      # SPI master TX / slave RX
├── vandal_espnow/   # ESP-NOW broadcast TX / RX
├── vandal_ble/      # NimBLE GATT server (master) / client (slave)
└── vandal_thread/   # OpenThread FTD, UDP multicast TX / RX
```

---

## 📋 Prerequisites

| Tool        | Version                              |
| ----------- | ------------------------------------ |
| **ESP-IDF** | v5.5.2                               |
| **Target**  | ESP32-C6                             |
| **pnpm**    | ≥ 10 (for the React dashboard build) |

The dashboard is pre-built in the `website/` folder. You only need pnpm if
you want to modify the React source (see [`vandal-monitor`](../vandal-monitor/)).

---

## 🚀 Build & Flash

```bash
# 1. Source the ESP-IDF environment
source ~/.espressif/v5.5.2/esp-idf/export.sh

# 2. Configure the role (default = MASTER)
idf.py menuconfig
#   → Vandal CTF Configuration → Board role → Master / Slave

# 3. Build
idf.py build

# 4. Flash (USB Serial/JTAG)
idf.py -p /dev/ttyACM0 flash

# 5. Monitor
idf.py -p /dev/ttyACM0 monitor
```

> **Important:** Erase NVS before first deployment (or after changing Thread
> network credentials) to avoid stale OpenThread datasets:
> ```bash
> idf.py -p /dev/ttyACM0 erase-flash && idf.py -p /dev/ttyACM0 flash
> ```

> **Tip:** Flash the master first, then change the role to _Slave_,
> rebuild, and flash the second board.

### Partition Layout

| Name       | Type | Offset   | Size     |
| ---------- | ---- | -------- | -------- |
| `nvs`      | NVS  | 0x9000   | 24 KB    |
| `phy_init` | PHY  | 0xF000   | 4 KB     |
| `factory`  | App  | 0x10000  | **3 MB** |
| `website`  | FAT  | 0x310000 | 960 KB   |

The `website` partition is auto-generated from the `website/` directory at
build time via `fatfs_create_spiflash_image()`.

---

## ⚙️ Configuration (KConfig)

All tunables are under **Vandal CTF Configuration** in `menuconfig`:

| Menu           | Key Options                              |
| -------------- | ---------------------------------------- |
| **Board role** | Master / Slave                           |
| **UART**       | TX/RX pins, baud rate                    |
| **I²C**        | SDA/SCL pins, slave address, clock       |
| **SPI**        | MOSI/MISO/SCLK/CS pins, clock speed      |
| **ESP-NOW**    | Channel, primary master key              |
| **BLE**        | Service UUIDs, PIN, device name          |
| **WiFi AP**    | SSID, password, channel, max connections |
| **HTTP**       | Port, master IP, slave POST interval     |
| **General**    | Payload send interval, LED GPIO          |

### Default Pin Mapping (ESP32-C6 DevKitC-1)

| Function                    | GPIO                     |
| --------------------------- | ------------------------ |
| UART TX / RX                | 16 / 17                  |
| I²C SDA / SCL               | 22 / 23                  |
| SPI MOSI / MISO / SCLK / CS | 18 / 20 / 19 / 21        |
| User LED                    | 15                       |
| Console                     | USB Serial/JTAG (native) |

---

## 🔌 HTTP API

All JSON endpoints. CORS enabled (`Access-Control-Allow-Origin: *`).

### `GET /status`

Returns a merged view: slave-received payloads from `POST /messages` merged
with any master-originated payloads (Thread TX, HTTP TX self-reported via the
event bus).

```json
{
  "UART":     "VANDAL payload #0042 sent through UART",
  "I2C":      "VANDAL payload #0042 sent through I2C",
  "SPI":      null,
  "ESP-NOW":  "VANDAL payload #0042 sent through ESP-NOW",
  "BLE-OPEN": "VANDAL payload #0042 sent through BLE-OPEN",
  "BLE-AUTH": "VANDAL payload #0042 sent through BLE-AUTH",
  "Thread":   "VANDAL payload #0042 sent through Thread",
  "HTTP":     "VANDAL payload #0042 sent through HTTP"
}
```

### `GET /system`

```json
{
  "heap_total": 327680,
  "heap_free": 215040,
  "heap_min_free": 198000,
  "temperature": 32.5,
  "uptime_s": 1234,
  "task_count": 14,
  "idf_version": "v5.5.2",
  "protocols": {
    "UART":     { "running": true,  "custom_payload": null },
    "BLE-AUTH": { "running": true,  "custom_payload": null },
    "Thread":   { "running": false, "custom_payload": null },
    ...
  }
}
```

### `POST /control`

```json
{ "protocol": "Thread", "action": "start" }
```

Response: `{ "status": "ok", "protocol": "Thread", "running": true }`

### `POST /payload`

```json
{ "protocol": "UART", "payload": "MY-CUSTOM-DATA" }
```

Send `"payload": null` or `""` to clear.

---

## 📁 Project Structure

```
vandal-ctf/
├── CMakeLists.txt          # Top-level project file
├── partitions.csv          # Custom partition table
├── sdkconfig.defaults      # Default KConfig values
├── main/
│   ├── main.c              # Entry point & boot orchestration
│   ├── Kconfig.projbuild   # All KConfig menus
│   └── CMakeLists.txt
├── components/             # Modular protocol drivers (see above)
└── website/                # Pre-built React dashboard (FATFS image)
```

---

## 📜 License

Educational / security research project — **vulnerable by design**.
Do **not** deploy in production.

