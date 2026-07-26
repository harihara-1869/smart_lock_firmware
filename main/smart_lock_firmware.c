/*
 * Smart Lock Firmware
 * Copyright (C) 2026 Harihara
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "comm_module.h"
#include "lli.h"

static const char *TAG = "TEST";

static bool no_provisioned_peer(size_t index, uint8_t pubkey_out[32],
                                void *ctx)
{
    (void)index;
    (void)pubkey_out;
    (void)ctx;
    return false;
}

static session_err_t application_stub(const uint8_t *plaintext_in,
                                      size_t len_in,
                                      uint8_t *plaintext_out,
                                      size_t *len_out,
                                      void *app_ctx)
{
    (void)plaintext_in;
    (void)len_in;
    (void)plaintext_out;
    (void)app_ctx;
    *len_out = 0;
    return SESSION_OK;
}

/* ── Hardware pin mapping (adjust to your board) ─────────────────── */
#define TEST_SDA_GPIO   8
#define TEST_SCL_GPIO   9
#define TEST_IRQ_GPIO   10
#define TEST_RST_GPIO   11
#define TEST_I2C_PORT   0

/* The former standalone LLI smoke-test helpers remain below for reference.
 * The firmware entrypoint now exercises the communication facade. */
#if 0
/* ── Card identity presented to readers ──────────────────────────── */
static const lli_config_t test_cfg = {
    .sda_gpio  = TEST_SDA_GPIO,
    .scl_gpio  = TEST_SCL_GPIO,
    .irq_gpio  = TEST_IRQ_GPIO,
    .rst_gpio  = TEST_RST_GPIO,
    .i2c_port  = TEST_I2C_PORT,

    .sens_res    = {0x04, 0x00},
    .nfcid1      = {0x01, 0x02, 0x03},
    .sel_res     = 0x20,            /* ISO14443-4 */
    .nfcid2      = {0x01, 0xFE, 0xA5, 0x01, 0x02, 0x03, 0x04, 0x05},
    .pad         = {0},
    .system_code = {0x88, 0xB4},
    .nfcid3t     = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A},
    .gt_len      = 0,
    .tk_len      = 0,
};

/* ── Test result tracking ────────────────────────────────────────── */
static int tests_run    = 0;
static int tests_passed = 0;

#define TEST_ASSERT(expr, name)                                         \
    do {                                                                \
        tests_run++;                                                    \
        if (expr) {                                                     \
            tests_passed++;                                             \
            ESP_LOGI(TAG, "  [PASS] %s", name);                        \
        } else {                                                        \
            ESP_LOGE(TAG, "  [FAIL] %s", name);                        \
        }                                                               \
    } while (0)

/* ── Individual function tests ───────────────────────────────────── */

/*
 * Test 1: lli_init — verify it produces a valid handle and returns OK.
 */
static lli_handle_t test_init(void)
{
    ESP_LOGI(TAG, "--- lli_init ---");

    /* NULL handle_out must fail. */
    lli_err_t err = lli_init(&test_cfg, NULL);
    TEST_ASSERT(err == LLI_ERR_INVALID_ARG, "init rejects NULL handle_out");

    /* NULL cfg must fail. */
    lli_handle_t h = NULL;
    err = lli_init(NULL, &h);
    TEST_ASSERT(err == LLI_ERR_INVALID_ARG, "init rejects NULL cfg");

    /* Valid call must succeed. */
    err = lli_init(&test_cfg, &h);
    TEST_ASSERT(err == LLI_OK, "init returns LLI_OK");
    TEST_ASSERT(h != NULL, "handle is non-NULL");

    return h;
}

/*
 * Test 2: lli_get_link_status — no reader present, expect RELEASED or ERROR.
 */
static void test_get_link_status(lli_handle_t h)
{
    ESP_LOGI(TAG, "--- lli_get_link_status (no reader) ---");

    lli_link_status_t st = lli_get_link_status(h);
    /* Without a reader we expect RELEASED (idle) or ERROR (bus issue).
     * ACTIVE would be wrong with no reader in the field. */
    TEST_ASSERT(st == LLI_STATUS_RELEASED || st == LLI_STATUS_ERROR,
                "status is RELEASED or ERROR without reader");
    TEST_ASSERT(st != LLI_STATUS_ACTIVE,
                "status is NOT ACTIVE without reader");

    /* NULL handle must return ERROR, not crash. */
    st = lli_get_link_status(NULL);
    TEST_ASSERT(st == LLI_STATUS_ERROR, "NULL handle returns STATUS_ERROR");
}

/*
 * Test 3: lli_activate — blocks until a phone taps the antenna.
 *         Returns the handle for subsequent APDU exchange tests.
 */
static lli_err_t test_activate(lli_handle_t h, uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "--- lli_activate (waiting for reader, %lu ms) ---",
             (unsigned long)timeout_ms);

    lli_err_t err = lli_activate(h, timeout_ms);
    if (err == LLI_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "  [INFO] No reader detected within timeout — "
                      "tap a phone and re-run, or increase timeout.");
        TEST_ASSERT(1, "activate returns TIMEOUT (expected if no phone)");
    } else if (err == LLI_OK) {
        TEST_ASSERT(1, "activate returns LLI_OK (phone detected)");
    } else {
        TEST_ASSERT(0, "activate unexpected error");
    }
    return err;
}

/*
 * Test 4: lli_receive_apdu — expects an APDU from the reader.
 *         Only meaningful after a successful activate.
 */
static lli_err_t test_receive_apdu(lli_handle_t h,
                                   uint8_t *buf, size_t buf_len,
                                   size_t *out_len)
{
    ESP_LOGI(TAG, "--- lli_receive_apdu ---");

    /* NULL args must fail. */
    lli_err_t err = lli_receive_apdu(NULL, buf, buf_len, out_len, 1000);
    TEST_ASSERT(err == LLI_ERR_INVALID_ARG, "receive_apdu rejects NULL handle");

    err = lli_receive_apdu(h, NULL, buf_len, out_len, 1000);
    TEST_ASSERT(err == LLI_ERR_INVALID_ARG, "receive_apdu rejects NULL buf");

    err = lli_receive_apdu(h, buf, buf_len, NULL, 1000);
    TEST_ASSERT(err == LLI_ERR_INVALID_ARG, "receive_apdu rejects NULL len_out");

    /* Actual receive — phone should send an APDU shortly after activation. */
    *out_len = 0;
    err = lli_receive_apdu(h, buf, buf_len, out_len, 30000);
    if (err == LLI_OK) {
        TEST_ASSERT(1, "receive_apdu returns LLI_OK");
        ESP_LOGI(TAG, "  received %u bytes:", (unsigned)*out_len);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, *out_len, ESP_LOG_INFO);
    } else {
        TEST_ASSERT(err == LLI_ERR_TIMEOUT || err == LLI_ERR_FRAME_INTEGRITY,
                    "receive_apdu returns TIMEOUT or FRAME_INTEGRITY");
    }
    return err;
}

/*
 * Test 5: lli_send_apdu — send a response back to the reader.
 */
static lli_err_t test_send_apdu(lli_handle_t h)
{
    ESP_LOGI(TAG, "--- lli_send_apdu ---");

    /* Generic ISO 7816 "success" status words (SW1=0x90, SW2=0x00). */
    const uint8_t sw[] = {0x90, 0x00};

    /* NULL handle must fail. */
    lli_err_t err = lli_send_apdu(NULL, sw, sizeof(sw), 2000);
    TEST_ASSERT(err == LLI_ERR_INVALID_ARG, "send_apdu rejects NULL handle");

    /* Valid send. */
    err = lli_send_apdu(h, sw, sizeof(sw), 2000);
    if (err == LLI_OK) {
        TEST_ASSERT(1, "send_apdu returns LLI_OK");
    } else {
        TEST_ASSERT(err == LLI_ERR_SEND_FAILED,
                    "send_apdu returns SEND_FAILED on error");
    }
    return err;
}

/*
 * Test 6: lli_get_link_status (with reader) — expect ACTIVE after
 *         successful activate + data exchange.
 */
static void test_get_link_status_active(lli_handle_t h)
{
    ESP_LOGI(TAG, "--- lli_get_link_status (after exchange) ---");

    lli_link_status_t st = lli_get_link_status(h);
    /* After a successful activate + APDU exchange the link should be ACTIVE.
     * It could also be RELEASED if the phone moved away already. */
    TEST_ASSERT(st == LLI_STATUS_ACTIVE || st == LLI_STATUS_RELEASED,
                "status is ACTIVE or RELEASED after exchange");
}

/*
 * Test 7: lli_abort — best-effort abort, must always return LLI_OK.
 */
static void test_abort(lli_handle_t h)
{
    ESP_LOGI(TAG, "--- lli_abort ---");

    /* NULL handle must fail. */
    lli_err_t err = lli_abort(NULL);
    TEST_ASSERT(err == LLI_ERR_INVALID_ARG, "abort rejects NULL handle");

    /* Valid abort — always returns LLI_OK. */
    err = lli_abort(h);
    TEST_ASSERT(err == LLI_OK, "abort returns LLI_OK");
}

/*
 * Test 8: lli_deinit — verify clean teardown.
 */
static void test_deinit(lli_handle_t h)
{
    ESP_LOGI(TAG, "--- lli_deinit ---");

    /* NULL handle is safe. */
    lli_err_t err = lli_deinit(NULL);
    TEST_ASSERT(err == LLI_OK, "deinit(NULL) returns LLI_OK");

    /* Valid deinit. */
    err = lli_deinit(h);
    TEST_ASSERT(err == LLI_OK, "deinit returns LLI_OK");
}

#endif

/* ── Entry point ─────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "=== communication module ===");

    comm_module_config_t cfg = {
        .sda_gpio = TEST_SDA_GPIO,
        .scl_gpio = TEST_SCL_GPIO,
        .irq_gpio = TEST_IRQ_GPIO,
        .rst_gpio = TEST_RST_GPIO,
        .i2c_port = TEST_I2C_PORT,
        .sens_res = {0x04, 0x00},
        .nfcid1 = {0x01, 0x02, 0x03},
        .sel_res = 0x20,
        .nfcid2 = {0x01, 0xFE, 0xA5, 0x01, 0x02, 0x03, 0x04, 0x05},
        .system_code = {0x88, 0xB4},
        .nfcid3t = {0x01, 0x02, 0x03, 0x04, 0x05,
                    0x06, 0x07, 0x08, 0x09, 0x0A},
        .peer_key_provider = no_provisioned_peer,
        .app_handler = application_stub,
    };

    comm_module_handle_t comm = NULL;
    if (comm_module_init(&cfg, &comm) != COMM_OK) {
        ESP_LOGE(TAG, "communication module init failed");
        return;
    }

    ESP_LOGI(TAG, "communication module ready; Application handler is a stub");
    for (;;) {
        comm_err_t comm_err = comm_module_run_once(comm);
        if (comm_err != COMM_OK && comm_err != COMM_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "communication session failed: %d", comm_err);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

#if 0
    ESP_LOGI(TAG, "=== LLI test suite ===");

    /* ---- init ---------------------------------------------------- */
    lli_handle_t h = test_init();
    if (!h) {
        ESP_LOGE(TAG, "init failed — aborting");
        return;
    }

    /* ---- link status (no reader) --------------------------------- */
    test_get_link_status(h);

    /* ---- activate ------------------------------------------------ */
    lli_err_t err = test_activate(h, 30000);
    if (err == LLI_OK) {
        /* ---- receive APDU ---------------------------------------- */
        uint8_t apdu_buf[256];
        size_t  apdu_len = 0;
        test_receive_apdu(h, apdu_buf, sizeof(apdu_buf), &apdu_len);

        /* ---- send APDU ------------------------------------------- */
        test_send_apdu(h);

        /* ---- link status (active session) ------------------------ */
        test_get_link_status_active(h);
    }

    /* ---- abort --------------------------------------------------- */
    test_abort(h);

    /* ---- teardown & re-init cycle -------------------------------- */
    ESP_LOGI(TAG, "--- deinit / re-init cycle ---");
    test_deinit(h);

    /* Re-init to prove clean teardown. */
    h = NULL;
    lli_err_t ri = lli_init(&test_cfg, &h);
    TEST_ASSERT(ri == LLI_OK && h != NULL, "re-init after deinit succeeds");
    if (h) {
        lli_deinit(h);
    }

    /* ---- summary ------------------------------------------------- */
    ESP_LOGI(TAG, "=== Results: %d/%d passed ===", tests_passed, tests_run);
#endif
}
