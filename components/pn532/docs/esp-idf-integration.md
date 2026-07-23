# ESP-IDF Integration

## Target Platform

- **Chip**: ESP32-S3 (dual-core Xtensa LX7, 240 MHz)
- **Framework**: ESP-IDF v5.4.4
- **Toolchain**: GCC (Xtensa)

## Component Structure

The PN532 driver is an ESP-IDF **project component** under `components/pn532/`.

### CMakeLists.txt

```cmake
idf_component_register(
    SRCS
        "src/pn532.c"
        "src/pn532_i2c.c"
        "src/pn532_cmd.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2c
    PRIV_REQUIRES
        esp_driver_gpio
        esp_common
        freertos
)
```

| Dependency | Type | Why |
|------------|------|-----|
| `esp_driver_i2c` | Public (`REQUIRES`) | Headers exposed in public API (`i2c_types.h` in `pn532_i2c.h`) |
| `esp_driver_gpio` | Private | GPIO config/ISR for IRQ pin (only in `.c` file) |
| `esp_common` | Private | `esp_err_t`, `esp_log.h` |
| `freertos` | Private | Mutexes, semaphores, tasks, delays |

**Note**: The component directory name "driver" would shadow ESP-IDF's built-in `driver` component. The directory is named `pn532` to avoid this.

## ESP-IDF APIs Used

### I2C Master (`driver/i2c_master.h`)

The driver uses ESP-IDF v5.x's **bus/device** I2C API (not the legacy `driver/i2c.h`).

| API | Where | Purpose |
|-----|-------|---------|
| `i2c_new_master_bus()` | `pn532_i2c_create` | Create I2C master bus with SDA/SCL pins, clock source |
| `i2c_master_bus_add_device()` | `pn532_i2c_create` | Register PN532 at address 0x24 on the bus |
| `i2c_master_probe()` | `pn532_i2c_create` | Verify PN532 is reachable; primes bus state machine |
| `i2c_master_transmit()` | `pn532_i2c_write` | Send frame bytes (START → addr(W) → data → STOP) |
| `i2c_master_receive()` | `pn532_i2c_read_status`, `pn532_i2c_read_frame` | Read bytes (START → addr(R) → data → STOP) |
| `i2c_master_bus_rm_device()` | `pn532_i2c_destroy` | Remove device from bus |
| `i2c_del_master_bus()` | `pn532_i2c_destroy` | Delete the I2C master bus |

#### Bus Configuration

```c
i2c_master_bus_config_t bus_cfg = {
    .i2c_port          = cfg->port,         // e.g. I2C_NUM_0
    .sda_io_num        = cfg->sda_gpio,     // e.g. GPIO 8
    .scl_io_num        = cfg->scl_gpio,     // e.g. GPIO 9
    .clk_source        = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,   // external pull-ups still recommended
};
```

#### Device Configuration

```c
i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x24,               // PN532 7-bit address
    .scl_speed_hz    = 400000,             // fast mode
    .scl_wait_us     = 50000,              // 50ms clock-stretch tolerance
};
```

### GPIO (`driver/gpio.h`)

Used for the optional IRQ pin (P70_IRQ, active-low), optional RST pin, and I2C bus recovery.

| API | Where | Purpose |
|-----|-------|---------|
| `gpio_config()` | `setup_irq`, reset pin setup | Configure GPIO pin mode, pull-up/down, interrupt type |
| `gpio_install_isr_service()` | `setup_irq` | Install GPIO ISR service (process-global, tolerates already installed) |
| `gpio_isr_handler_add()` | `setup_irq` | Attach ISR to IRQ pin |
| `gpio_get_level()` | `wait_ready_irq`, `recover_i2c_bus` | Read GPIO level (IRQ pre-check, SDA stuck detection) |
| `gpio_set_level()` | `pn532_i2c_reset_device`, `recover_i2c_bus` | Drive GPIO HIGH/LOW (reset pulse, SCL bit-bang) |
| `gpio_set_direction()` | `recover_i2c_bus` | Temporarily reconfigure GPIO mode for bus recovery |
| `gpio_isr_handler_remove()` | `pn532_i2c_destroy` | Detach ISR during cleanup |

#### IRQ GPIO Configuration

```c
gpio_config_t io = {
    .pin_bit_mask = 1ULL << irq_gpio,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,     // IRQ is open-drain, active-low
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_NEGEDGE,      // trigger on high→low transition
};
```

### FreeRTOS

#### Mutexes (`freertos/semphr.h`)

| Primitive | Created In | Purpose |
|-----------|------------|---------|
| `xSemaphoreCreateMutex()` | `pn532_i2c_create` | Bus serialisation — every I2C transaction locks/unlocks this |
| `xSemaphoreTake()` | `bus_lock`, `wait_ready_irq` | Acquire mutex (5s timeout) or wait for IRQ semaphore |
| `xSemaphoreGive()` | `bus_unlock` | Release bus mutex |
| `xSemaphoreCreateBinary()` | `setup_irq` | IRQ signalling — starts empty, given by ISR on falling edge |
| `xSemaphoreGiveFromISR()` | `pn532_irq_isr` | Give IRQ semaphore from interrupt context |
| `vSemaphoreDelete()` | `pn532_i2c_destroy` | Clean up semaphores |

### ROM Delays (`esp_rom_sys.h`)

Used for precise microsecond delays during I2C bus recovery bit-banging.

| API | Where | Purpose |
|-----|-------|---------|
| `esp_rom_delay_us()` | `recover_i2c_bus` | 50µs half-periods for SCL pulses during bus recovery |

This function is available on all ESP32 targets, has no FreeRTOS dependency, and is safe in any context (ISR or task).

#### Task Management (`freertos/task.h`)

| API | Where | Purpose |
|-----|-------|---------|
| `vTaskDelay(pdMS_TO_TICKS(1))` | `pn532_i2c_write` | 1ms backoff between write retries |
| `vTaskDelay(pdMS_TO_TICKS(1))` | `wait_ready_poll` | 1ms delay between status polls |
| `vTaskDelay(pdMS_TO_TICKS(2))` | `pn532_wakeup` | 2ms oscillator stabilisation (T_osc_start) |

#### ISR Context

The GPIO ISR is placed in IRAM for low-latency execution:

```c
static void IRAM_ATTR pn532_irq_isr(void *arg) {
    // ...
    xSemaphoreGiveFromISR(c->irq_sem, &hp_task_woken);
    if (hp_task_woken) portYIELD_FROM_ISR();
}
```

`portYIELD_FROM_ISR()` ensures a context switch to the higher-priority waiting task immediately.

### Logging (`esp_log.h`)

All modules use the `PN532` tag:

```c
static const char *TAG = "PN532";
ESP_LOGI(TAG, "I2C transport ready (IRQ mode, gpio %d, %"PRIu32" Hz)", ...);
ESP_LOGE(TAG, "write failed after %d retries: %s", ...);
ESP_LOGW(TAG, "read_frame: status not ready (0x%02x)", ...);
ESP_LOGD(TAG, "status read failed: %s", ...);
```

Log levels used:
- `ESP_LOGE` — bus errors, frame validation failures, allocation failures
- `ESP_LOGW` — non-fatal issues (status not ready, wakeup NACK)
- `ESP_LOGI` — lifecycle events (init, probe detection, transport ready)
- `ESP_LOGD` — status read failures during polling (noisy, debug-only)

### Error Handling (`esp_err.h`)

All public functions return `esp_err_t`. Common error codes:

| Code | Meaning |
|------|---------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL pointer or invalid parameter |
| `ESP_ERR_NO_MEM` | Allocation failure |
| `ESP_ERR_TIMEOUT` | Bus timeout, mutex timeout, ACK timeout, or readiness timeout |
| `ESP_ERR_INVALID_RESPONSE` | Malformed frame (no start code, bad TFI, truncated) |
| `ESP_ERR_INVALID_CRC` | LCS or DCS checksum mismatch |
| `ESP_ERR_INVALID_SIZE` | Response payload too large for output buffer |
| `ESP_FAIL` | Generic failure (error frame from PN532, ACK mismatch) |

`esp_err_to_name()` is used in all error log messages for human-readable names.

## Build Configuration

The project uses the default ESP-IDF sdkconfig for ESP32-S3 with:

- I2C driver enabled (default in ESP-IDF v5.x)
- GPIO driver enabled (default)
- FreeRTOS on both cores
- Log level INFO (change to DEBUG for verbose I2C logging)
- Console on UART0 at 115200 baud + USB Serial/JTAG secondary

## Flashing

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Or manually:

```bash
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 2MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/pn532_driver.bin
```
