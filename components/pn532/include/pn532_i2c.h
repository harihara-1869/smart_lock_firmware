/**
 * @file pn532_i2c.h
 * @brief I2C transport backend for the PN532 driver.
 *
 * Creates a @ref pn532_transport_ops_t vtable bound to an I2C bus, ready to be
 * handed to @ref pn532_init via @ref pn532_config_t. The backend owns the I2C
 * master bus/device it creates and (optionally) a GPIO IRQ line.
 *
 * ## I2C address — 8-bit datasheet value vs 7-bit ESP-IDF value
 * The PN532 datasheet quotes 0x48 (write) and 0x49 (read). Those are 8-bit
 * "wire" addresses that already include the R/W bit in bit 0. ESP-IDF's I2C
 * driver takes the 7-bit address WITHOUT the R/W bit, which is 0x48 >> 1 = 0x24.
 * This backend uses 0x24 everywhere; the driver appends the R/W bit for you.
 *
 * @note ESP-IDF v5.x, uses the `driver/i2c_master.h` (bus/device) API.
 */

#pragma once

#include <stdint.h>

#include "driver/i2c_types.h"  /* i2c_port_t / i2c_port_num_t */
#include "esp_err.h"
#include "pn532.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PN532 7-bit I2C address (see file header for the 8-bit vs 7-bit note).
 */
#define PN532_I2C_ADDRESS       0x24

/**
 * @brief Default I2C clock: 400 kHz fast mode, the PN532's documented maximum.
 */
#define PN532_I2C_DEFAULT_CLK_HZ  400000U

/**
 * @brief I2C transport configuration.
 *
 * GPIO pins are owned by the application — this component does NOT hardcode or
 * default any pin mapping. @p sda_gpio and @p scl_gpio are mandatory; set
 * @p irq_gpio to -1 to select polling mode instead of IRQ mode.
 */
typedef struct {
    int        sda_gpio;   /**< SDA GPIO number (required; set by application). */
    int        scl_gpio;   /**< SCL GPIO number (required; set by application). */
    int        irq_gpio;   /**< IRQ (P70_IRQ) GPIO number; -1 if not wired -> polling mode. */
    int        rst_gpio;   /**< RST GPIO number; -1 if not wired -> no hardware reset. */
    i2c_port_t port;       /**< I2C port number (e.g. I2C_NUM_0). */
    uint32_t   clk_speed;  /**< SCL frequency in Hz; 0 -> PN532_I2C_DEFAULT_CLK_HZ (400 kHz). */
} pn532_i2c_config_t;

/**
 * @brief Create an I2C transport for the PN532.
 *
 * Installs the I2C master bus and device using the caller-supplied GPIOs and
 * clock, configures the optional IRQ GPIO (with a GPIO ISR that gives a binary
 * semaphore), creates the bus mutex, and fills @p ops_out / @p ctx_out ready for
 * @ref pn532_init. If @p cfg->clk_speed is 0 the default 400 kHz is used.
 *
 * @param[in]  cfg      Transport configuration (see @ref pn532_i2c_config_t).
 * @param[out] ops_out  Receives the transport vtable (copied by value; caller
 *                      may store it on the stack until passed to pn532_init).
 * @param[out] ctx_out  Receives the transport-private context pointer.
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_ARG on NULL args or an invalid SDA/SCL GPIO
 *   - ESP_ERR_NO_MEM if allocation fails
 *   - otherwise an esp_err_t from the I2C or GPIO driver
 *
 * @note On failure, no resources are leaked and @p ctx_out is left unchanged.
 */
esp_err_t pn532_i2c_create(const pn532_i2c_config_t *cfg,
                           pn532_transport_ops_t *ops_out,
                           void **ctx_out);

/**
 * @brief Destroy an I2C transport created by @ref pn532_i2c_create.
 *
 * Tears down the IRQ ISR, the I2C device/bus and the mutex, then frees @p ctx.
 * Normally invoked for you through @ref pn532_deinit (the vtable's `destroy`);
 * call directly only if you never handed the context to @ref pn532_init.
 *
 * @param[in] ctx Transport context from @ref pn532_i2c_create (may be NULL).
 */
void pn532_i2c_destroy(void *ctx);

/**
 * @brief Assert hardware reset on the PN532.
 *
 * Drives rst_gpio LOW for @p pulse_ms milliseconds, then HIGH.
 * After releasing reset, waits @p settle_ms milliseconds for the PN532
 * oscillator to stabilise before returning.
 *
 * Typical values: pulse_ms = 10, settle_ms = 100.
 *
 * If rst_gpio was configured as -1, returns ESP_ERR_NOT_SUPPORTED
 * immediately without touching the bus.
 *
 * The caller is responsible for re-running SAMConfiguration after reset.
 *
 * @param ctx        I2C transport context (the void* returned by
 *                   pn532_i2c_create).
 * @param pulse_ms   Duration to hold reset LOW (milliseconds).
 * @param settle_ms  Wait after reset releases before returning (ms).
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED if no rst_gpio configured.
 */
esp_err_t pn532_i2c_reset_device(void *ctx,
                                  uint32_t pulse_ms,
                                  uint32_t settle_ms);

#ifdef __cplusplus
}
#endif
