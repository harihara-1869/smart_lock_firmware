# I2C Transport Layer (`pn532_i2c.c`)

## Overview

Implements the `pn532_transport_ops_t` vtable over ESP-IDF v5.x's `driver/i2c_master.h` (bus/device API). Handles all PN532 I2C-specific quirks.

**Source**: `components/pn532/src/pn532_i2c.c`
**Header**: `components/pn532/include/pn532_i2c.h`

## I2C Address

The PN532 datasheet quotes 0x48 (write) and 0x49 (read) — these are 8-bit "wire" addresses including the R/W bit. ESP-IDF's I2C driver takes the 7-bit address without the R/W bit:

```
0x48 >> 1 = 0x24
```

The driver appends the R/W bit automatically. Defined as `PN532_I2C_ADDRESS` (0x24).

## Internal Context (`pn532_i2c_ctx_t`)

```c
typedef struct {
    i2c_master_bus_handle_t bus;        // ESP-IDF I2C master bus handle
    i2c_master_dev_handle_t dev;        // ESP-IDF I2C device handle (PN532)
    SemaphoreHandle_t       mutex;      // serialises all bus transactions

    int  irq_gpio;                      // -1 => polling mode
    int  rst_gpio;                      // -1 => no hardware reset
    int  sda_gpio;                      // stored for bus recovery
    int  scl_gpio;                      // stored for bus recovery
    bool irq_mode;                      // irq_gpio >= 0
    bool isr_added;                     // ISR handler registered (for cleanup)
    SemaphoreHandle_t irq_sem;          // binary sem, given from GPIO ISR

    uint32_t poll_interval_ms;          // polling interval (1ms default)
    uint32_t poll_max_retries;          // polling retry cap (1000 default)
} pn532_i2c_ctx_t;
```

## Initialisation Sequence (`pn532_i2c_create`)

Creates the full I2C transport in this order:

1. **Validate** — NULL checks, SDA/SCL GPIOs must be >= 0
2. **Allocate context** — `calloc` zero-initialises all handles
3. **Create mutex** — `xSemaphoreCreateMutex()` for bus serialisation
4. **Create I2C master bus** — `i2c_new_master_bus()` with:
   - `i2c_port`: caller-specified (e.g. `I2C_NUM_0`)
   - `sda_io_num` / `scl_io_num`: caller-specified GPIOs
   - `clk_source`: `I2C_CLK_SRC_DEFAULT`
   - `glitch_ignore_cnt`: 7 (standard)
   - `enable_internal_pullup`: true (external 4.7k pull-ups still recommended)
5. **Add PN532 device** — `i2c_master_bus_add_device()` with:
   - `device_address`: 0x24 (7-bit)
   - `scl_speed_hz`: caller-specified or 400 kHz default
   - `scl_wait_us`: 50,000 (50ms clock-stretch tolerance)
   - `dev_addr_length`: `I2C_ADDR_BIT_LEN_7`
6. **Probe** — `i2c_master_probe()` verifies the PN532 responds at 0x24. This also primes the I2C bus state machine (without it, the first `i2c_master_transmit` can fail with `ESP_ERR_INVALID_STATE`).
7. **Setup IRQ** (if `irq_gpio >= 0`) — configures GPIO, installs ISR, adds handler
8. **Return** — fills `ops_out` with the vtable, `ctx_out` with the context

### IRQ GPIO Setup (`setup_irq`)

```c
gpio_config_t io = {
    .pin_bit_mask = 1ULL << irq_gpio,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,    // IRQ is open-drain, active-low
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_NEGEDGE,     // assertion = high → low
};
gpio_config(&io);
gpio_install_isr_service(0);               // process-global, tolerates already installed
gpio_isr_handler_add(irq_gpio, pn532_irq_isr, ctx);
```

## Tunable Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `PN532_WRITE_MAX_RETRIES` | 5 | Max write attempts on address NACK |
| `PN532_WRITE_RETRY_DELAY_MS` | 1 | Delay between write retries |
| `PN532_POLL_INTERVAL_MS` | 1 | Status polling interval |
| `PN532_POLL_MAX_RETRIES` | 1000 | Max polling iterations |
| `PN532_MUTEX_TIMEOUT_MS` | 5000 | Bus lock acquisition timeout |
| `PN532_XFER_TIMEOUT_MS` | 50 | Per-byte I2C transaction timeout |
| `PN532_IRQ_ASSERTED_LEVEL` | 0 | IRQ is active-low |

## Bus Mutex

Every I2C transaction goes through `bus_lock()` / `bus_unlock()`:

```c
static esp_err_t bus_lock(pn532_i2c_ctx_t *c) {
    if (xSemaphoreTake(c->mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
static void bus_unlock(pn532_i2c_ctx_t *c) {
    xSemaphoreGive(c->mutex);
}
```

The 5-second timeout accommodates long PN532 operations like `TgInitAsTarget` which can block for several seconds waiting for an external reader.

## Vtable Operations

### `write` — `pn532_i2c_write`

Sends a complete frame to the PN532 over I2C.

```
I2C transaction: START → addr(W) → frame bytes → STOP
```

- Acquires bus mutex
- Retries up to 5 times on failure (address NACK is common after prior exchanges)
- 1ms backoff between retries
- On exhaustion: returns `ESP_ERR_TIMEOUT` (not the raw bus error)

**ESP-IDF API**: `i2c_master_transmit(dev, buf, len, timeout_ms)`

### `read_status` — `pn532_i2c_read_status`

Reads the 1-byte PN532 status byte. Used by polling-mode `wait_ready`.

```
I2C transaction: START → addr(R) → 1 byte → STOP
```

- A "not-ready" read (RDY bit = 0) completes normally — the STOP after a not-ready status is the correct PN532 behaviour.
- Returns the raw status byte in `*status_out`.

**ESP-IDF API**: `i2c_master_receive(dev, buf, 1, timeout_ms)`

### `read_frame` — `pn532_i2c_read_frame`

Reads status byte + frame bytes in a **single** I2C transaction.

```
I2C transaction: START → addr(R) → (1 + N) bytes → STOP
```

- `buf[0]` = status byte, `buf[1..len-1]` = frame bytes
- **Critical**: must be one transaction. Issuing STOP before the full frame is clocked out causes the PN532 to drop remaining bytes.
- Returns `ESP_ERR_INVALID_RESPONSE` if the status byte reports not-ready (caller should have waited first).

**ESP-IDF API**: `i2c_master_receive(dev, buf, len, timeout_ms)`

### `wait_ready` — `pn532_i2c_wait_ready`

Blocks until the PN532 signals a frame is ready. Dispatches to IRQ or polling mode based on `ctx->irq_mode`.

#### IRQ Mode (`wait_ready_irq`)

1. **Pre-check**: `gpio_get_level(irq_gpio)` — IRQ may already be asserted
2. **Block**: `xSemaphoreTake(irq_sem, timeout)` — ISR gives the semaphore on falling edge
3. **Post-check**: re-read GPIO level to catch a race between assertion and timeout

```c
// ISR (runs in IRAM)
static void IRAM_ATTR pn532_irq_isr(void *arg) {
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)arg;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(c->irq_sem, &hp_task_woken);
    if (hp_task_woken) portYIELD_FROM_ISR();
}
```

#### Polling Mode (`wait_ready_poll`)

Loops: read status byte → check RDY bit → delay 1ms → repeat. Bounded by both `timeout_ms` and `poll_max_retries` (1000).

**ESP-IDF APIs**: `gpio_get_level()`, `xSemaphoreTake()`, `xSemaphoreGiveFromISR()`

## Hardware Reset (`pn532_i2c_reset_device`)

Asserts hardware reset on the PN532 by driving the RST GPIO pin.

**Sequence**:
1. Check if `rst_gpio` is configured (-1 means no reset pin) — return `ESP_ERR_NOT_SUPPORTED` if not
2. Drive RST LOW (`gpio_set_level(rst_gpio, 0)`)
3. Wait `pulse_ms` milliseconds (typically 10ms)
4. Drive RST HIGH (`gpio_set_level(rst_gpio, 1)`)
5. Wait `settle_ms` milliseconds for oscillator stabilisation (typically 100ms)

**Note**: the caller is responsible for re-running SAMConfiguration after reset. When called via `pn532_reset()`, the bus mutex is held during the entire reset sequence.

## I2C Bus Recovery (`recover_i2c_bus`)

Detects and recovers from a stuck I2C bus where the PN532 is holding SDA low (e.g. after a power glitch, partial transaction, or firmware hang).

### Detection

Before attempting recovery, checks if SDA is actually stuck low:
1. Temporarily reconfigure SDA GPIO as input (no pull)
2. Read level via `gpio_get_level()`
3. If SDA is high, restore I2C function and return ESP_OK immediately — bus is not stuck

### Recovery Sequence (NXP AN10609 / IEEE procedure)

If SDA is stuck low:

1. **Configure SCL as output** for bit-banging
2. **Clock out up to 9 SCL pulses** with 50µs half-periods
   - Check SDA after each rising edge
   - Stop early if SDA goes high (device released the bus)
3. **Issue manual STOP condition**: SCL high, SDA low → SDA high
4. **Restore both pins** to I2C peripheral function

### Post-Recovery

After successful recovery:
- **If rst_gpio available**: call `pn532_i2c_reset_device(10, 100)` to reset PN532 firmware to clean state
- **If no rst_gpio**: send wakeup sequence (0x55 bytes) and log a warning:
  `"PN532: no reset pin — PN532 internal state may be undefined after recovery"`

### Integration with Write Retry Loop

Bus recovery is called only after the full write retry budget is exhausted:

```
pn532_i2c_write():
  for attempt in 1..5:
    i2c_master_transmit() → if OK, return ESP_OK
    vTaskDelay(1ms)
  
  // All retries exhausted
  ESP_LOGW("write failed after 5 retries, attempting bus recovery")
  
  // Check if SDA is stuck and attempt recovery
  bus_lock()
  recover_i2c_bus()
  bus_unlock()
  
  // One final write attempt after recovery
  bus_lock()
  err = i2c_master_transmit()
  bus_unlock()
  
  if err != ESP_OK:
    return ESP_ERR_TIMEOUT
```

**Note**: recovery is NOT called on every failed transaction — only after the full retry budget is exhausted.

## Destruction (`pn532_i2c_destroy`)

Tears down in reverse order, NULL-safe at each step:

1. `gpio_isr_handler_remove()` — if ISR was added
2. `vSemaphoreDelete(irq_sem)` — if IRQ semaphore exists
3. `i2c_master_bus_rm_device(dev)` — if device handle exists
4. `i2c_del_master_bus(bus)` — if bus handle exists
5. `vSemaphoreDelete(mutex)` — if mutex exists
6. `free(ctx)` — free the context struct

## Clock Stretching

The PN532 stretches SCL after recognising its address (without H_REQ wired). The driver handles this via:

- `scl_wait_us = 50000` (50ms) in the device config — tells the ESP-IDF I2C hardware how long to tolerate SCL being held low
- `PN532_XFER_TIMEOUT_MS = 50` — software timeout per transaction

Both values are generous enough for normal operation while staying within ESP32-S3 register limits.
