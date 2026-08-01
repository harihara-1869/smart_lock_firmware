#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "comm_module.h"
#include "provision_mgr.h"
#include "nvs_store.h"
#include "mock_phone.h"
#include "lli_mock.h"

static const char *TAG = "TEST_INTEGRATION";

/* Provided by smart_lock_firmware.c */
extern const uint8_t TEST_LOCK_PUBLIC_KEY[32];
extern const uint8_t TEST_LOCK_SECRET_KEY[64];
extern const uint8_t TEST_PHONE_PUBLIC_KEY[32];
extern const uint8_t TEST_PHONE_SECRET_KEY[64];

static void run_mock_phone_interaction(mock_phone_t *phone, bool is_provisioning)
{
    uint8_t m1[64];
    mock_phone_get_m1(phone, m1);

    /* Send M1 (INS=0x10) */
    uint8_t capdu_m1[5 + 64] = { 0x00, 0x10, 0x00, 0x00, 64 };
    memcpy(&capdu_m1[5], m1, 64);
    lli_mock_push_rx(capdu_m1, sizeof(capdu_m1));

    /* Receive M2 */
    uint8_t rapdu_m2[256];
    size_t rapdu_m2_len;
    if (!lli_mock_pull_tx(rapdu_m2, &rapdu_m2_len, 5000)) {
        ESP_LOGE(TAG, "Test failed: timeout waiting for M2");
        return;
    }
    if (rapdu_m2_len != 128 + 2 || rapdu_m2[128] != 0x90 || rapdu_m2[129] != 0x00) {
        ESP_LOGE(TAG, "Test failed: invalid M2 response");
        return;
    }

    /* Process M2 and generate M3 */
    uint8_t m3[64];
    if (!mock_phone_process_m2_get_m3(phone, rapdu_m2, m3)) {
        ESP_LOGE(TAG, "Test failed: mock phone failed to process M2 (invalid lock signature)");
        return;
    }

    /* Send M3 (INS=0x11) */
    uint8_t capdu_m3[5 + 64] = { 0x00, 0x11, 0x00, 0x00, 64 };
    memcpy(&capdu_m3[5], m3, 64);
    lli_mock_push_rx(capdu_m3, sizeof(capdu_m3));

    /* Receive M3 ACK */
    uint8_t rapdu_m3_ack[256];
    size_t rapdu_m3_ack_len;
    if (!lli_mock_pull_tx(rapdu_m3_ack, &rapdu_m3_ack_len, 5000)) {
        ESP_LOGE(TAG, "Test failed: timeout waiting for M3 ACK");
        return;
    }
    
    if (rapdu_m3_ack_len != 2 || rapdu_m3_ack[0] != 0x90 || rapdu_m3_ack[1] != 0x00) {
        ESP_LOGE(TAG, "Test failed: invalid M3 ACK: %02x%02x", rapdu_m3_ack[0], rapdu_m3_ack[1]);
        return;
    }
    
    ESP_LOGI(TAG, "Handshake successful!");

    if (is_provisioning) {
        /* Encrypt CMD_PROVISION payload */
        uint8_t provision_payload[65];
        provision_payload[0] = 0x01; // CMD_PROVISION
        memcpy(&provision_payload[1], provision_mgr_get_secret(), 32);
        memcpy(&provision_payload[33], TEST_PHONE_PUBLIC_KEY, 32);

        uint8_t encrypted[256];
        size_t encrypted_len;
        mock_phone_encrypt_payload(phone, provision_payload, sizeof(provision_payload), encrypted, &encrypted_len);

        /* Send Secure Payload (INS=0x20) */
        uint8_t capdu_sec[5 + 256] = { 0x00, 0x20, 0x00, 0x00, (uint8_t)encrypted_len };
        memcpy(&capdu_sec[5], encrypted, encrypted_len);
        lli_mock_push_rx(capdu_sec, 5 + encrypted_len);

        /* Receive Secure Response */
        uint8_t rapdu_sec[256];
        size_t rapdu_sec_len;
        if (!lli_mock_pull_tx(rapdu_sec, &rapdu_sec_len, 5000)) {
            ESP_LOGE(TAG, "Test failed: timeout waiting for Secure Payload ACK");
            return;
        }

        if (rapdu_sec_len < 2 || rapdu_sec[rapdu_sec_len - 2] != 0x90 || rapdu_sec[rapdu_sec_len - 1] != 0x00) {
            ESP_LOGE(TAG, "Test failed: invalid Secure Payload ACK");
            return;
        }

        uint8_t decrypted[256];
        size_t decrypted_len;
        if (!mock_phone_decrypt_payload(phone, rapdu_sec, rapdu_sec_len - 2, decrypted, &decrypted_len)) {
            ESP_LOGE(TAG, "Test failed: failed to decrypt response");
            return;
        }

        if (decrypted_len == 2 && decrypted[0] == 0x90 && decrypted[1] == 0x00) {
            ESP_LOGI(TAG, "Provisioning Success!");
        } else {
            ESP_LOGE(TAG, "Provisioning Failed! Application Response: %02x", decrypted[0]);
        }
    }
}

void test_integration_run(void *arg)
{
    ESP_LOGI(TAG, "--- Starting Integration Test ---");
    
    mock_phone_t phone;
    mock_phone_init(&phone, TEST_PHONE_SECRET_KEY, TEST_LOCK_PUBLIC_KEY);
    
    /* Provisioning Flow */
    ESP_LOGI(TAG, "Arming provisioning window...");
    provision_mgr_arm(5000);
    lli_mock_trigger_activation();
    
    run_mock_phone_interaction(&phone, true);
    
    if (nvs_store_is_key_authorized(TEST_PHONE_PUBLIC_KEY)) {
        ESP_LOGI(TAG, "Key successfully stored in NVS.");
    } else {
        ESP_LOGE(TAG, "Test failed: key NOT stored in NVS.");
    }
    
    ESP_LOGI(TAG, "--- Integration Test Complete ---");
}
