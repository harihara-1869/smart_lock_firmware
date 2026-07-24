#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lli.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRANSPORT_STATE_IDLE,
    TRANSPORT_STATE_ACTIVATED,
    TRANSPORT_STATE_HANDSHAKE,
    TRANSPORT_STATE_SECURE_SESSION,
    TRANSPORT_STATE_RELEASED,
} transport_state_t;

typedef enum {
    TRANSPORT_OK                    = 0,
    TRANSPORT_ERR_TIMEOUT           = 1,
    TRANSPORT_ERR_LINK_LOST         = 2,
    TRANSPORT_ERR_INVALID_STATE     = 3,
    TRANSPORT_ERR_INVALID_APDU      = 4,
    TRANSPORT_ERR_PAYLOAD_TOO_LARGE = 5,
    TRANSPORT_ERR_INTERNAL          = 6,
} transport_err_t;

typedef struct {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    uint8_t lc;
    uint8_t data[255];
} transport_capdu_t;

typedef struct {
    uint8_t data[256];
    uint8_t len;
    uint8_t sw1;
    uint8_t sw2;
} transport_rapdu_t;

typedef transport_err_t (*transport_apdu_handler_t)(
    const transport_capdu_t *capdu,
    transport_rapdu_t *rapdu,
    void *user_ctx
);

typedef void (*transport_erase_handler_t)(void *user_ctx);

typedef struct {
    lli_config_t    lli_cfg;
    uint32_t        handshake_timeout_ms;
    uint32_t        activate_timeout_ms;
    uint32_t        apdu_timeout_ms;
    transport_apdu_handler_t  on_apdu;
    transport_erase_handler_t on_erase;
    void           *user_ctx;
} transport_config_t;

typedef struct transport_t *transport_handle_t;

transport_err_t transport_init(const transport_config_t *cfg,
                               transport_handle_t *handle_out);
transport_err_t transport_deinit(transport_handle_t handle);

transport_err_t transport_run_session(transport_handle_t handle);

transport_state_t transport_get_state(transport_handle_t handle);

#ifdef __cplusplus
}
#endif
