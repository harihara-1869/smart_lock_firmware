/**
 * @file pn532_i2c.c
 * @brief I2C transport backend for the PN532 driver (ESP-IDF v5.x, i2c_master).
 *
 * Implements the @ref pn532_transport_ops_t vtable over the new bus/device I2C
 * API. Handles the PN532-specific quirks:
 *
 *  - Status-byte protocol: every read starts with a 1-byte status whose bit 0
 *    (RDY) says whether a frame is available. A not-ready read must be stopped
 *    immediately; a ready read must clock out the whole frame in ONE
 *    transaction (an early STOP loses the remaining bytes). Because the
 *    i2c_master API brackets each receive with its own START/STOP, the ready
 *    path over-reads status+frame together in a single i2c_master_receive().
 *  - Dual-mode ready detection: hardware IRQ (P70, active-low, via a GPIO ISR
 *    that gives a binary semaphore) when irq_gpio >= 0, else status-byte polling.
 *  - Address-NACK retry on writes (the chip may NACK right after a prior
 *    exchange) and native clock-stretch tolerance via a generous scl_wait_us.
 *  - A single FreeRTOS mutex serialises every bus transaction.
 */

#include "pn532_i2c.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "PN532";

/* ---- Tunables ----------------------------------------------------------- */

/* PN532 status byte, bit 0 = RDY (1 => a frame is ready to read). */
#define PN532_STATUS_RDY_BIT    0x01

/* Write address-NACK retry policy. */
#define PN532_WRITE_MAX_RETRIES 5
#define PN532_WRITE_RETRY_DELAY_MS 1

/* Polling-mode defaults (used when irq_gpio < 0). */
#define PN532_POLL_INTERVAL_MS  1
#define PN532_POLL_MAX_RETRIES  1000

/* Bus mutex acquisition timeout — TgInitAsTarget etc. can block a long time. */
#define PN532_MUTEX_TIMEOUT_MS  5000

/*
 * Per-byte I2C transaction timeout for the ESP-IDF driver. Without H_REQ wired
 * the PN532 stretches SCL after recognising its address; the driver handles
 * stretching natively but needs a generous ceiling. 50 ms is ample and stays
 * well within the ESP32-S3 SCL-timeout register range (no runtime clamp).
 */
#define PN532_XFER_TIMEOUT_MS   50

/* IRQ line is active-low: level 0 means "frame ready / IRQ asserted". */
#define PN532_IRQ_ASSERTED_LEVEL 0

/* ---- Transport context -------------------------------------------------- */

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t       mutex;      /* serialises all bus transactions   */

    int  irq_gpio;                      /* -1 => polling mode                */
    int  rst_gpio;                      /* -1 => no hardware reset           */
    int  sda_gpio;                      /* stored for bus recovery           */
    int  scl_gpio;                      /* stored for bus recovery           */
    bool irq_mode;                      /* irq_gpio >= 0                     */
    bool isr_added;                     /* isr handler registered for cleanup */
    SemaphoreHandle_t irq_sem;          /* given from the GPIO ISR           */

    uint32_t poll_interval_ms;
    uint32_t poll_max_retries;
} pn532_i2c_ctx_t;

/* ---- Forward declarations ----------------------------------------------- */

static esp_err_t recover_i2c_bus(pn532_i2c_ctx_t *ctx);
esp_err_t pn532_i2c_reset_device(void *ctx, uint32_t pulse_ms, uint32_t settle_ms);

/* ---- Mutex helpers ------------------------------------------------------ */

static esp_err_t bus_lock(pn532_i2c_ctx_t *c)
{
    if (xSemaphoreTake(c->mutex, pdMS_TO_TICKS(PN532_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "bus mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void bus_unlock(pn532_i2c_ctx_t *c)
{
    xSemaphoreGive(c->mutex);
}

/* Thin wrappers matching the vtable signature (void* ctx). */
static esp_err_t vtable_bus_lock(void *ctx, uint32_t timeout_ms)
{
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)ctx;
    if (xSemaphoreTake(c->mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "bus mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void vtable_bus_unlock(void *ctx)
{
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)ctx;
    xSemaphoreGive(c->mutex);
}

/* ---- GPIO ISR ----------------------------------------------------------- */

static void IRAM_ATTR pn532_irq_isr(void *arg)
{
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)arg;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(c->irq_sem, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ------------------------------------------------------------------------- */
/* Transport ops                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Write a frame; retry while the chip NACKs its own address.
 *
 * The i2c_master transmit issues START -> addr(W) -> data -> STOP atomically and
 * fails if the address is NACKed. The PN532 may NACK briefly right after a prior
 * exchange, so we retry up to PN532_WRITE_MAX_RETRIES with a short delay.
 */
static esp_err_t pn532_i2c_write(void *ctx, const uint8_t *buf, size_t len)
{
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)ctx;

    esp_err_t err = bus_lock(c);
    if (err != ESP_OK) {
        return err;
    }

    err = ESP_FAIL;
    for (int attempt = 0; attempt < PN532_WRITE_MAX_RETRIES; attempt++) {
        err = i2c_master_transmit(c->dev, buf, len, PN532_XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            break;
        }
        /* Address/data NACK or bus busy — back off briefly and retry. */
        vTaskDelay(pdMS_TO_TICKS(PN532_WRITE_RETRY_DELAY_MS));
    }

    bus_unlock(c);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write failed after %d retries, attempting bus recovery",
                 PN532_WRITE_MAX_RETRIES);

        /* Attempt bus recovery before giving up. */
        esp_err_t recovery_err = bus_lock(c);
        if (recovery_err == ESP_OK) {
            recover_i2c_bus(c);
            bus_unlock(c);
        }

        /* One final attempt after recovery. */
        err = bus_lock(c);
        if (err == ESP_OK) {
            err = i2c_master_transmit(c->dev, buf, len, PN532_XFER_TIMEOUT_MS);
            bus_unlock(c);
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write failed even after bus recovery: %s",
                     esp_err_to_name(err));
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

/**
 * @brief Read a single status byte in a self-contained transaction.
 *
 * Used by polling-mode wait_ready. If the chip is not ready the read still
 * completes (START -> addr(R) -> 1 byte -> STOP); the STOP after a not-ready
 * status is exactly the required behaviour.
 */
static esp_err_t pn532_i2c_read_status(void *ctx, uint8_t *status_out)
{
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)ctx;

    esp_err_t err = bus_lock(c);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_receive(c->dev, status_out, 1, PN532_XFER_TIMEOUT_MS);
    bus_unlock(c);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "status read failed: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief Read status byte + frame bytes together in ONE transaction.
 *
 * @p buf[0] receives the status byte, @p buf[1..len-1] the frame. The whole
 * read is a single START -> addr(R) -> len bytes -> STOP so no bytes are lost
 * to an early STOP. If the status byte reports not-ready, returns
 * ESP_ERR_INVALID_RESPONSE (the caller should have waited for readiness first).
 */
static esp_err_t pn532_i2c_read_frame(void *ctx, uint8_t *buf, size_t len)
{
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)ctx;

    esp_err_t err = bus_lock(c);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_receive(c->dev, buf, len, PN532_XFER_TIMEOUT_MS);
    bus_unlock(c);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame read failed: %s", esp_err_to_name(err));
        return err;
    }

    if ((buf[0] & PN532_STATUS_RDY_BIT) == 0) {
        ESP_LOGW(TAG, "read_frame: status not ready (0x%02x)", buf[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/**
 * @brief IRQ-mode readiness wait: pre-check level, then block on the semaphore.
 */
static esp_err_t wait_ready_irq(pn532_i2c_ctx_t *c, uint32_t timeout_ms)
{
    /* Pre-check: the IRQ may already be asserted before we arm the semaphore. */
    if (gpio_get_level(c->irq_gpio) == PN532_IRQ_ASSERTED_LEVEL) {
        return ESP_OK;
    }

    if (xSemaphoreTake(c->irq_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }

    /* Final level check to catch an assertion that raced the timeout. */
    if (gpio_get_level(c->irq_gpio) == PN532_IRQ_ASSERTED_LEVEL) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Polling-mode readiness wait: repeatedly read the 1-byte status.
 *
 * Bounded by both the caller timeout and the configured retry cap; on either
 * exhaustion returns ESP_ERR_TIMEOUT.
 */
static esp_err_t wait_ready_poll(pn532_i2c_ctx_t *c, uint32_t timeout_ms)
{
    const uint32_t interval = c->poll_interval_ms;
    uint32_t elapsed = 0;

    for (uint32_t iter = 0; iter < c->poll_max_retries; iter++) {
        uint8_t status = 0;
        esp_err_t err = pn532_i2c_read_status(c, &status);
        if (err == ESP_OK && (status & PN532_STATUS_RDY_BIT)) {
            return ESP_OK;
        }
        if (elapsed >= timeout_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(interval));
        elapsed += interval;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t pn532_i2c_wait_ready(void *ctx, uint32_t timeout_ms)
{
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)ctx;
    return c->irq_mode ? wait_ready_irq(c, timeout_ms)
                       : wait_ready_poll(c, timeout_ms);
}

/* ------------------------------------------------------------------------- */
/* I2C bus recovery                                                           */
/* ------------------------------------------------------------------------- */

#include "esp_rom_sys.h"
#include "soc/gpio_reg.h"

/**
 * @brief Recover an I2C bus stuck with SDA held low.
 *
 * Implements the NXP AN10609 / IEEE recovery procedure: clock out up to 9
 * SCL pulses until SDA releases, then issue a STOP condition.
 *
 * @param ctx  I2C transport context.
 * @return ESP_OK if SDA is high after recovery (bus free), ESP_FAIL otherwise.
 */
static esp_err_t recover_i2c_bus(pn532_i2c_ctx_t *ctx)
{
    /* Check if SDA is actually stuck low. */
    gpio_set_direction(ctx->sda_gpio, GPIO_MODE_INPUT);
    int level = gpio_get_level(ctx->sda_gpio);
    if (level == 1) {
        /* SDA is high — bus is not stuck. Restore I2C function. */
        esp_rom_gpio_connect_out_signal(ctx->sda_gpio, 0, false, false);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "SDA stuck low, attempting bus recovery");

    /* Configure SCL as output for bit-banging. */
    gpio_set_direction(ctx->scl_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(ctx->scl_gpio, 1);
    esp_rom_delay_us(5);

    /* Clock out up to 9 pulses, checking SDA after each rising edge. */
    for (int i = 0; i < 9; i++) {
        gpio_set_level(ctx->scl_gpio, 0);
        esp_rom_delay_us(50);
        gpio_set_level(ctx->scl_gpio, 1);
        esp_rom_delay_us(50);

        level = gpio_get_level(ctx->sda_gpio);
        if (level == 1) {
            ESP_LOGI(TAG, "SDA released after %d SCL pulses", i + 1);
            break;
        }
    }

    /* Issue STOP: SCL high, SDA low → SDA high. */
    gpio_set_direction(ctx->sda_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(ctx->sda_gpio, 0);
    esp_rom_delay_us(5);
    gpio_set_level(ctx->scl_gpio, 1);
    esp_rom_delay_us(5);
    gpio_set_level(ctx->sda_gpio, 1);
    esp_rom_delay_us(5);

    /* Restore pins to I2C peripheral function. */
    esp_rom_gpio_connect_out_signal(ctx->sda_gpio, 0, false, false);
    esp_rom_gpio_connect_in_signal(ctx->sda_gpio, 0, false);
    esp_rom_gpio_connect_out_signal(ctx->scl_gpio, 0, false, false);
    esp_rom_gpio_connect_in_signal(ctx->scl_gpio, 0, false);

    /* Verify SDA is now high. */
    gpio_set_direction(ctx->sda_gpio, GPIO_MODE_INPUT);
    level = gpio_get_level(ctx->sda_gpio);
    if (level != 1) {
        ESP_LOGE(TAG, "bus recovery failed: SDA still low");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2C bus recovery successful");

    /* Reset PN532 if we have a reset pin. */
    if (ctx->rst_gpio != -1) {
        pn532_i2c_reset_device(ctx, 10, 100);
    } else {
        ESP_LOGW(TAG, "PN532: no reset pin — PN532 internal state may be undefined after recovery");
        /* Send wakeup sequence to try to restore PN532 state. */
        static const uint8_t wake[] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
        i2c_master_transmit(ctx->dev, wake, sizeof(wake), PN532_XFER_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Hardware reset                                                             */
/* ------------------------------------------------------------------------- */

esp_err_t pn532_i2c_reset_device(void *ctx, uint32_t pulse_ms, uint32_t settle_ms)
{
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)ctx;

    if (c == NULL || c->rst_gpio < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "asserting hardware reset (gpio %d, pulse %"PRIu32" ms, settle %"PRIu32" ms)",
             c->rst_gpio, pulse_ms, settle_ms);

    /* Drive RST LOW. */
    gpio_set_level(c->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(pulse_ms));

    /* Release RST (HIGH). */
    gpio_set_level(c->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));

    ESP_LOGI(TAG, "hardware reset complete");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Construction / destruction                                                 */
/* ------------------------------------------------------------------------- */

/* Shared vtable — const, no per-instance state lives here. */
static const pn532_transport_ops_t s_pn532_i2c_ops = {
    .write        = pn532_i2c_write,
    .read_status  = pn532_i2c_read_status,
    .read_frame   = pn532_i2c_read_frame,
    .wait_ready   = pn532_i2c_wait_ready,
    .destroy      = pn532_i2c_destroy,
    .reset_device = pn532_i2c_reset_device,
    .bus_lock     = vtable_bus_lock,
    .bus_unlock   = vtable_bus_unlock,
};

/**
 * @brief Configure the optional IRQ GPIO and its ISR-driven semaphore.
 */
static esp_err_t setup_irq(pn532_i2c_ctx_t *c)
{
    c->irq_sem = xSemaphoreCreateBinary();
    if (c->irq_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << c->irq_gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* IRQ is open-drain, active low */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,    /* assertion = high -> low       */
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    /* The ISR service is process-global; tolerate it already being installed. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(c->irq_gpio, pn532_irq_isr, c);
    if (err != ESP_OK) {
        return err;
    }
    c->isr_added = true;
    return ESP_OK;
}

esp_err_t pn532_i2c_create(const pn532_i2c_config_t *cfg,
                           pn532_transport_ops_t *ops_out,
                           void **ctx_out)
{
    if (cfg == NULL || ops_out == NULL || ctx_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->sda_gpio < 0 || cfg->scl_gpio < 0) {
        ESP_LOGE(TAG, "SDA/SCL GPIOs must be set by the application");
        return ESP_ERR_INVALID_ARG;
    }

    pn532_i2c_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    c->irq_gpio         = cfg->irq_gpio;
    c->rst_gpio         = cfg->rst_gpio;
    c->sda_gpio         = cfg->sda_gpio;
    c->scl_gpio         = cfg->scl_gpio;
    c->irq_mode         = (cfg->irq_gpio >= 0);
    c->poll_interval_ms = PN532_POLL_INTERVAL_MS;
    c->poll_max_retries = PN532_POLL_MAX_RETRIES;

    esp_err_t err;

    c->mutex = xSemaphoreCreateMutex();
    if (c->mutex == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* --- I2C master bus --- */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = cfg->port,
        .sda_io_num        = cfg->sda_gpio,
        .scl_io_num        = cfg->scl_gpio,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,  /* external pull-ups still recommended */
        },
    };
    err = i2c_new_master_bus(&bus_cfg, &c->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* --- PN532 device on the bus (7-bit address 0x24; see header note) --- */
    const uint32_t clk = (cfg->clk_speed != 0) ? cfg->clk_speed
                                               : PN532_I2C_DEFAULT_CLK_HZ;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PN532_I2C_ADDRESS,
        .scl_speed_hz    = clk,
        .scl_wait_us     = PN532_XFER_TIMEOUT_MS * 1000U,  /* clock-stretch tolerance */
    };
    err = i2c_master_bus_add_device(c->bus, &dev_cfg, &c->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Probe the bus to verify the PN532 is present and to initialise the
     * bus state machine (the first transmit after add_device can fail with
     * ESP_ERR_INVALID_STATE without a preceding probe). */
    err = i2c_master_probe(c->bus, PN532_I2C_ADDRESS, PN532_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PN532 not found on I2C bus at 0x%02x: %s",
                 PN532_I2C_ADDRESS, esp_err_to_name(err));
        goto fail;
    }
    ESP_LOGI(TAG, "PN532 detected on I2C bus at 0x%02x", PN532_I2C_ADDRESS);

    /* --- Optional IRQ line --- */
    if (c->irq_mode) {
        err = setup_irq(c);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IRQ GPIO setup failed: %s", esp_err_to_name(err));
            goto fail;
        }
        ESP_LOGI(TAG, "I2C transport ready (IRQ mode, gpio %d, %"PRIu32" Hz)",
                 c->irq_gpio, clk);
    } else {
        ESP_LOGI(TAG, "I2C transport ready (polling mode, %"PRIu32" Hz)", clk);
    }

    /* --- Optional reset line --- */
    if (c->rst_gpio >= 0) {
        const gpio_config_t rst_io = {
            .pin_bit_mask = 1ULL << c->rst_gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&rst_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "reset GPIO setup failed: %s", esp_err_to_name(err));
            goto fail;
        }
        /* Start with reset released (HIGH). */
        gpio_set_level(c->rst_gpio, 1);
        ESP_LOGI(TAG, "reset pin configured (gpio %d)", c->rst_gpio);
    }

    *ops_out = s_pn532_i2c_ops;
    *ctx_out = c;
    return ESP_OK;

fail:
    pn532_i2c_destroy(c);
    return err;
}

void pn532_i2c_destroy(void *ctx)
{
    if (ctx == NULL) {
        return;
    }
    pn532_i2c_ctx_t *c = (pn532_i2c_ctx_t *)ctx;

    if (c->isr_added) {
        gpio_isr_handler_remove(c->irq_gpio);
    }
    if (c->irq_sem != NULL) {
        vSemaphoreDelete(c->irq_sem);
    }
    if (c->dev != NULL) {
        i2c_master_bus_rm_device(c->dev);
    }
    if (c->bus != NULL) {
        i2c_del_master_bus(c->bus);
    }
    if (c->mutex != NULL) {
        vSemaphoreDelete(c->mutex);
    }
    free(c);
}
