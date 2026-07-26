/* Smart Lock communication facade. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_types.h"
#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMM_OK = 0,
    COMM_ERR_TIMEOUT,
    COMM_ERR_INVALID_ARG,
    COMM_ERR_INTERNAL,
} comm_err_t;

typedef session_err_t (*comm_app_command_handler_t)(
        const uint8_t *plaintext_in, size_t len_in,
        uint8_t *plaintext_out, size_t *len_out,
        void *app_ctx);

typedef struct {
    int sda_gpio;
    int scl_gpio;
    int irq_gpio;
    int rst_gpio;
    i2c_port_t i2c_port;
    uint32_t i2c_clk_hz;

    uint8_t sens_res[2];
    uint8_t nfcid1[3];
    uint8_t sel_res;
    uint8_t nfcid2[8];
    uint8_t pad[8];
    uint8_t system_code[2];
    uint8_t nfcid3t[10];
    uint8_t gt[47];
    uint8_t gt_len;
    uint8_t tk[47];
    uint8_t tk_len;

    uint32_t handshake_timeout_ms;
    uint32_t activate_timeout_ms;
    uint32_t apdu_timeout_ms;

    uint8_t local_sk[64];
    uint8_t local_pk[32];
    session_peer_key_provider_t peer_key_provider;
    void *peer_key_provider_ctx;

    comm_app_command_handler_t app_handler;
    void *app_handler_ctx;
} comm_module_config_t;

typedef struct comm_module_t *comm_module_handle_t;

comm_err_t comm_module_init(const comm_module_config_t *cfg,
                            comm_module_handle_t *out);
comm_err_t comm_module_deinit(comm_module_handle_t handle);
comm_err_t comm_module_run_once(comm_module_handle_t handle);
comm_err_t comm_module_run_forever(comm_module_handle_t handle);
bool comm_module_is_session_active(comm_module_handle_t handle);
comm_err_t comm_module_force_abort(comm_module_handle_t handle);

#ifdef __cplusplus
}
#endif
