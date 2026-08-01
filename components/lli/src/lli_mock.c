#ifdef MOCK_LLI_FOR_TESTING

#include "lli.h"
#include "lli_mock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static SemaphoreHandle_t s_rx_sem = NULL;
static uint8_t s_rx_buf[256];
static size_t s_rx_len = 0;

static SemaphoreHandle_t s_tx_sem = NULL;
static uint8_t s_tx_buf[256];
static size_t s_tx_len = 0;

static SemaphoreHandle_t s_activate_sem = NULL;

struct lli_t {
    int dummy;
};

static struct lli_t s_dummy_lli;

void lli_mock_trigger_activation(void) {
    if (s_activate_sem) xSemaphoreGive(s_activate_sem);
}

void lli_mock_push_rx(const uint8_t *data, size_t len) {
    if (len > sizeof(s_rx_buf)) return;
    memcpy(s_rx_buf, data, len);
    s_rx_len = len;
    if (s_rx_sem) xSemaphoreGive(s_rx_sem);
}

bool lli_mock_pull_tx(uint8_t *out_data, size_t *out_len, uint32_t timeout_ms) {
    if (xSemaphoreTake(s_tx_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        memcpy(out_data, s_tx_buf, s_tx_len);
        *out_len = s_tx_len;
        return true;
    }
    return false;
}

lli_err_t lli_init(const lli_config_t *cfg, lli_handle_t *handle_out) {
    if (!s_rx_sem) s_rx_sem = xSemaphoreCreateBinary();
    if (!s_tx_sem) s_tx_sem = xSemaphoreCreateBinary();
    if (!s_activate_sem) s_activate_sem = xSemaphoreCreateBinary();
    *handle_out = &s_dummy_lli;
    return LLI_OK;
}

lli_err_t lli_deinit(lli_handle_t handle) { return LLI_OK; }

lli_err_t lli_activate(lli_handle_t handle, uint32_t timeout_ms) {
    if (xSemaphoreTake(s_activate_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return LLI_OK;
    }
    return LLI_ERR_TIMEOUT;
}

lli_err_t lli_receive_apdu(lli_handle_t handle, uint8_t *buf, size_t buf_len,
                           size_t *len_out, uint32_t timeout_ms) {
    if (xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        if (s_rx_len > buf_len) return LLI_ERR_INTERNAL;
        memcpy(buf, s_rx_buf, s_rx_len);
        *len_out = s_rx_len;
        return LLI_OK;
    }
    return LLI_ERR_TIMEOUT;
}

lli_err_t lli_send_apdu(lli_handle_t handle, const uint8_t *data,
                        size_t len, uint32_t timeout_ms) {
    if (len > sizeof(s_tx_buf)) return LLI_ERR_INTERNAL;
    memcpy(s_tx_buf, data, len);
    s_tx_len = len;
    xSemaphoreGive(s_tx_sem);
    return LLI_OK;
}

lli_link_status_t lli_get_link_status(lli_handle_t handle) {
    return LLI_STATUS_ACTIVE;
}

lli_err_t lli_abort(lli_handle_t handle) {
    /* Wake up any waiting semaphores? Optional for simple tests. */
    if (s_activate_sem) xSemaphoreGive(s_activate_sem);
    if (s_rx_sem) xSemaphoreGive(s_rx_sem);
    return LLI_OK;
}

#endif /* MOCK_LLI_FOR_TESTING */
