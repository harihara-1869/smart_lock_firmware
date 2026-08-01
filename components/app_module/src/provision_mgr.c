#include "provision_mgr.h"
#include "app_cmd.h"
#include "app_display.h"
#include "nvs_store.h"
#include "comm_module.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "PROV_MGR";

static bool    s_provisioning_active = false;
static int64_t s_provisioning_deadline_us = 0;
static uint8_t s_provision_secret[32];

/* Constant-time memory comparison */
static int constant_time_memcmp(const void *a, const void *b, size_t len) {
    const uint8_t *a_ptr = (const uint8_t *)a;
    const uint8_t *b_ptr = (const uint8_t *)b;
    uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= a_ptr[i] ^ b_ptr[i];
    }
    return result == 0 ? 0 : 1;
}

void provision_mgr_arm(uint32_t timeout_ms)
{
    esp_fill_random(s_provision_secret, sizeof(s_provision_secret));
    
    s_provisioning_deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    s_provisioning_active = true;
    
    if (comm_module_arm_provisioning_window(timeout_ms) != COMM_OK) {
        ESP_LOGE(TAG, "Failed to arm comm module");
        s_provisioning_active = false;
        return;
    }
    
    app_display_show_qr(s_provision_secret);
    ESP_LOGI(TAG, "Provisioning armed for %lu ms", (unsigned long)timeout_ms);
}

bool provision_mgr_is_active(void)
{
    if (s_provisioning_active && esp_timer_get_time() > s_provisioning_deadline_us) {
        s_provisioning_active = false;
        app_display_clear();
        ESP_LOGI(TAG, "Provisioning window expired");
    }
    return s_provisioning_active;
}

void provision_mgr_handle_cmd(const uint8_t *cmd_bytes, size_t len,
                              uint8_t *resp_bytes, size_t *resp_len)
{
    if (!provision_mgr_is_active()) {
        ESP_LOGE(TAG, "Provisioning not active or expired");
        comm_module_force_abort();
        *resp_len = 0;
        return;
    }
    
    if (len == 0) {
        ESP_LOGE(TAG, "Empty command");
        comm_module_force_abort();
        s_provisioning_active = false;
        app_display_clear();
        *resp_len = 0;
        return;
    }
    
    uint8_t opcode = cmd_bytes[0];
    if (opcode != CMD_PROVISION) {
        ESP_LOGE(TAG, "Invalid opcode during provisioning: 0x%02X", opcode);
        comm_module_force_abort();
        s_provisioning_active = false;
        app_display_clear();
        *resp_len = 0;
        return;
    }
    
    /* Payload: opcode(1) + secret(32) + claimed_pk(32) = 65 bytes */
    if (len != 65) {
        ESP_LOGE(TAG, "Invalid PROVISION payload length: %zu", len);
        comm_module_force_abort();
        s_provisioning_active = false;
        app_display_clear();
        *resp_len = 0;
        return;
    }
    
    const uint8_t *received_secret = &cmd_bytes[1];
    const uint8_t *claimed_pk = &cmd_bytes[33];
    
    if (!comm_module_provision_verify_identity(claimed_pk)) {
        ESP_LOGE(TAG, "Provisioning identity verification failed");
        comm_module_force_abort();
        s_provisioning_active = false;
        app_display_clear();
        *resp_len = 0;
        return;
    }
    
    if (constant_time_memcmp(received_secret, s_provision_secret, 32) != 0) {
        ESP_LOGE(TAG, "Provisioning secret mismatch");
        comm_module_force_abort();
        s_provisioning_active = false;
        app_display_clear();
        *resp_len = 0;
        return;
    }
    
    if (nvs_store_add_key(claimed_pk)) {
        ESP_LOGI(TAG, "Provisioning successful, key added to NVS");
        resp_bytes[0] = 0x90;
        resp_bytes[1] = 0x00;
        *resp_len = 2;
    } else {
        ESP_LOGE(TAG, "Failed to store key in NVS");
        comm_module_force_abort();
        *resp_len = 0;
    }
    
    s_provisioning_active = false;
    app_display_clear();
}

const uint8_t* provision_mgr_get_secret(void)
{
    return s_provision_secret;
}
