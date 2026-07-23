# Architecture

## Layered Design

The driver is split into three layers connected by vtables. The core layer is transport-agnostic; the command layer builds on top of the core; the I2C backend is swappable.

```
┌─────────────────────────────────────────────────┐
│              Application (main/)                │
│         pn532_driver.c — test suite             │
└────────────────────┬────────────────────────────┘
                     │ calls pn532_get_firmware_version / pn532_tg_init_as_target / etc.
                     ▼
┌─────────────────────────────────────────────────┐
│         Command Layer (pn532_cmd.c)             │
│                                                 │
│  GetFirmwareVersion    SAMConfiguration         │
│  SetParameters         TgInitAsTarget           │
│  TgGetData             TgSetData                │
│  InListPassiveTarget   TgGetInitiatorCommand    │
│  TgResponseToInitiator  TgSetMetaData           │
│  pn532_error_code_t (19 PN532 error codes)      │
└────────────────────┬────────────────────────────┘
                     │ calls pn532_send_command / pn532_receive_response
                     ▼
┌─────────────────────────────────────────────────┐
│           Core Driver (pn532.c)                 │
│                                                 │
│  Frame builder    Frame parser    ACK handler   │
│  pn532_build_frame()  pn532_parse_frame()       │
│  pn532_send_command()                           │
│  pn532_receive_response()                       │
│  pn532_wakeup()   pn532_reset()                 │
│  resync_frame() (internal)                      │
└────────────────────┬────────────────────────────┘
                     │ calls ops->write / ops->read_frame / ops->wait_ready
                     ▼
┌─────────────────────────────────────────────────┐
│      Transport Vtable (pn532_transport_ops_t)   │
│                                                 │
│  .write()  .read_status()  .read_frame()        │
│  .wait_ready()  .destroy()                      │
│  .reset_device()  .bus_lock()  .bus_unlock()    │
└────────────────────┬────────────────────────────┘
                     │ implemented by
                     ▼
┌─────────────────────────────────────────────────┐
│        I2C Backend (pn532_i2c.c)                │
│                                                 │
│  i2c_master_transmit / i2c_master_receive       │
│  GPIO ISR (IRQ mode) or status-byte polling     │
│  FreeRTOS mutex (bus serialisation)             │
│  Hardware reset (optional RST GPIO)             │
│  I2C bus recovery (SDA stuck-low)               │
└─────────────────────────────────────────────────┘
```

## Module Responsibilities

### pn532_cmd.c — Command Layer

High-level wrappers that each send one PN532 command and parse its response. Knows the PN532 command set but not the frame format or transport. Owns:

- **GetFirmwareVersion (0x02)**: queries IC code, firmware version/revision, and capability bitmask. Validates IC == 0x32.
- **SAMConfiguration (0x14)**: sets the SAM operating mode (NORMAL, VIRTUAL_CARD, WIRED_CARD, DUAL_CARD) and IRQ behaviour. Only VIRTUAL_CARD mode sends the timeout byte.
- **SetParameters (0x12)**: sets communication flags (NAD, DID, auto-ATR/RATS, ISO14443-4 PICC, remove preamble). Rejects RFU bits 3 and 7.
- **TgInitAsTarget (0x8C)**: puts the PN532 into card-emulation mode. Blocks until an external RF reader activates the chip or timeout elapses. Builds the MifareParams + FeliCaParams + NFCID3t + optional Gt/Tk payload.
- **TgGetData (0x86)**: receives data from the NFC initiator. Handles MI-bit chaining transparently (accumulates fragments up to PN532_MAX_PAYLOAD_LEN).
- **TgSetData (0x8E)**: sends data to the NFC initiator. Max 262 bytes per exchange.
- **InListPassiveTarget (0x4A)**: discovers passive NFC targets in the RF field. Supports Type A (106 kbps), FeliCa (212/424 kbps), Type B (106 kbps), and Jewel. Parses ATQA, SAK, NFCID, and ATS for each target.
- **TgGetInitiatorCommand (0x88)**: retrieves the raw command from the NFC initiator in ISO14443-4 PICC emulation mode.
- **TgResponseToInitiator (0x90)**: sends a response to the initiator in ISO14443-4 PICC emulation. Alternative to TgSetData for the initial response after RATS.
- **TgSetMetaData (0x94)**: sets meta-data to be appended to the next TgResponseToInitiator/TgSetData payload.

All functions validate PN532 error status bytes using the `pn532_error_code_t` enum (19 error codes from 0x00 to 0x29).

### pn532.c — Core Driver

Knows the PN532 frame format and nothing about I2C/SPI/UART. Owns:

- **Frame building**: serialises cmd_code + params into a PN532 information frame (normal or extended) with preamble, start code, LEN, LCS, TFI, DATA, DCS, postamble.
- **Frame parsing**: locates start code, validates LCS/DCS checksums, checks TFI (0xD5), extracts DATA payload. Detects error frames (TFI 0x7F).
- **Command/response handshake**: sends frame, waits for ACK (6-byte fixed pattern), validates it. For responses: waits for readiness, over-reads frame, validates, sends NACK on CRC failure to request retransmit.
- **Frame resync**: on checksum mismatch, drains up to 512 incoming bytes looking for a valid preamble sequence (0x00 0x00 0xFF), then retries the receive exactly once.
- **Wakeup**: sends 0x55 burst to wake the chip from Power-Down, waits for oscillator stabilisation.
- **Hardware reset**: delegates to transport's `reset_device` callback while holding the bus mutex (optional, requires RST GPIO).

### pn532_i2c.c — I2C Transport Backend

Implements the `pn532_transport_ops_t` vtable over ESP-IDF's `i2c_master` API. Owns:

- **I2C bus and device lifecycle**: creates master bus, adds PN532 device at address 0x24, probes to verify presence.
- **Bus mutex**: a FreeRTOS mutex serialises all I2C transactions (5-second timeout). Exposed via `bus_lock`/`bus_unlock` vtable callbacks for `pn532_reset`.
- **Write with retry**: retries up to 5 times on address NACK (the PN532 may NACK briefly after a prior exchange).
- **I2C bus recovery**: detects SDA stuck-low after write retry exhaustion. Bit-bangs SCL (up to 9 pulses) to unstick the bus, issues a manual STOP condition, then resets the PN532 if a reset pin is available.
- **Status-byte protocol**: single-byte status read for polling; combined status+frame read in one transaction to avoid losing bytes to an early STOP.
- **Ready detection**: IRQ mode (GPIO ISR gives binary semaphore) or polling mode (repeated status reads at 1ms intervals).
- **Hardware reset**: optional RST GPIO pin. Configured as output HIGH on init. `pn532_i2c_reset_device()` drives LOW for pulse_ms, then HIGH, waits settle_ms.

## Data Flow: Send Command → Get Response

```
Application
  │
  ├─ pn532_send_command(h, 0x02, NULL, 0)
  │   │
  │   ├─ pn532_build_frame(0x02, NULL, 0)
  │   │   → [0x00 0x00 0xFF 0x02 0xFE 0xD4 0x02 0x2A 0x00]
  │   │
  │   ├─ tp_write(frame)  ──►  I2C: START → 0x24(W) → frame → STOP
  │   │
  │   └─ pn532_read_ack()
  │       ├─ tp_wait_ready()  ──  block on IRQ semaphore or poll status
  │       └─ tp_read_frame()  ──  I2C: START → 0x24(R) → 7 bytes → STOP
  │           → validate: [0x00 0x00 0xFF 0x00 0xFF 0x00] = ACK
  │
  └─ pn532_receive_response(h, buf, 16, &len, 1000)
      │
      ├─ tp_wait_ready(1000)
      │   └─ IRQ fires → semaphore given → returns ESP_OK
      │
      ├─ tp_read_frame(raw, 277)  ──  over-read status + worst-case frame
      │
      └─ pn532_parse_frame(raw)
          ├─ skip status byte
          ├─ find start code (0x00 0xFF)
          ├─ read LEN + validate LCS
          ├─ read TFI (expect 0xD5) + DATA + DCS, validate checksum
          └─ copy DATA into application buffer
              → buf = [0x03, 0x32, 0x01, 0x06, 0x07] (GetFirmwareVersion response)
```

## Thread Safety

- The I2C backend uses a single FreeRTOS mutex (`bus_lock`/`bus_unlock`) around every I2C transaction. This serialises access from multiple FreeRTOS tasks.
- The core driver does not add its own locking — it assumes one logical user per handle.
- The GPIO ISR (`pn532_irq_isr`) runs in IRAM and uses `xSemaphoreGiveFromISR` to signal the wait_ready function.
