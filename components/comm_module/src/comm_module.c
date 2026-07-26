/* Smart Lock communication facade. */
#include "comm_module.h"

#include <stdlib.h>
#include <string.h>

#include "transport.h"

#define DEFAULT_HANDSHAKE_TIMEOUT_MS 2000U
#define DEFAULT_ACTIVATE_TIMEOUT_MS  30000U
#define DEFAULT_APDU_TIMEOUT_MS      2000U

struct comm_module_t {
    session_handle_t session;
    transport_handle_t transport;
};

static comm_err_t map_transport_error(transport_err_t err)
{
    if (err == TRANSPORT_OK) {
        return COMM_OK;
    }
    if (err == TRANSPORT_ERR_TIMEOUT) {
        return COMM_ERR_TIMEOUT;
    }
    if (err == TRANSPORT_ERR_INVALID_APDU ||
        err == TRANSPORT_ERR_INVALID_STATE) {
        return COMM_ERR_INVALID_ARG;
    }
    return COMM_ERR_INTERNAL;
}

comm_err_t comm_module_init(const comm_module_config_t *cfg,
                            comm_module_handle_t *out)
{
    if (!cfg || !out || !cfg->peer_key_provider || !cfg->app_handler) {
        return COMM_ERR_INVALID_ARG;
    }

    struct comm_module_t *h = calloc(1, sizeof(*h));
    if (!h) {
        return COMM_ERR_INTERNAL;
    }

    session_config_t session_cfg = {0};
    memcpy(session_cfg.local_sk, cfg->local_sk, sizeof(session_cfg.local_sk));
    memcpy(session_cfg.local_pk, cfg->local_pk, sizeof(session_cfg.local_pk));
    session_cfg.peer_key_provider = cfg->peer_key_provider;
    session_cfg.peer_key_provider_ctx = cfg->peer_key_provider_ctx;
    session_cfg.app_handler = cfg->app_handler;
    session_cfg.app_handler_ctx = cfg->app_handler_ctx;

    if (session_init(&session_cfg, &h->session) != SESSION_OK) {
        free(h);
        return COMM_ERR_INTERNAL;
    }

    transport_config_t transport_cfg = {0};
    transport_cfg.lli_cfg.sda_gpio = cfg->sda_gpio;
    transport_cfg.lli_cfg.scl_gpio = cfg->scl_gpio;
    transport_cfg.lli_cfg.irq_gpio = cfg->irq_gpio;
    transport_cfg.lli_cfg.rst_gpio = cfg->rst_gpio;
    transport_cfg.lli_cfg.i2c_port = cfg->i2c_port;
    transport_cfg.lli_cfg.i2c_clk_hz = cfg->i2c_clk_hz;
    memcpy(transport_cfg.lli_cfg.sens_res, cfg->sens_res, 2);
    memcpy(transport_cfg.lli_cfg.nfcid1, cfg->nfcid1, 3);
    transport_cfg.lli_cfg.sel_res = cfg->sel_res;
    memcpy(transport_cfg.lli_cfg.nfcid2, cfg->nfcid2, 8);
    memcpy(transport_cfg.lli_cfg.pad, cfg->pad, 8);
    memcpy(transport_cfg.lli_cfg.system_code, cfg->system_code, 2);
    memcpy(transport_cfg.lli_cfg.nfcid3t, cfg->nfcid3t, 10);
    memcpy(transport_cfg.lli_cfg.gt, cfg->gt, 47);
    transport_cfg.lli_cfg.gt_len = cfg->gt_len;
    memcpy(transport_cfg.lli_cfg.tk, cfg->tk, 47);
    transport_cfg.lli_cfg.tk_len = cfg->tk_len;

    transport_cfg.handshake_timeout_ms = cfg->handshake_timeout_ms ?
        cfg->handshake_timeout_ms : DEFAULT_HANDSHAKE_TIMEOUT_MS;
    transport_cfg.activate_timeout_ms = cfg->activate_timeout_ms ?
        cfg->activate_timeout_ms : DEFAULT_ACTIVATE_TIMEOUT_MS;
    transport_cfg.apdu_timeout_ms = cfg->apdu_timeout_ms ?
        cfg->apdu_timeout_ms : DEFAULT_APDU_TIMEOUT_MS;
    transport_cfg.on_apdu = session_on_apdu;
    transport_cfg.on_erase = session_on_erase;
    transport_cfg.user_ctx = h->session;

    if (transport_init(&transport_cfg, &h->transport) != TRANSPORT_OK) {
        session_deinit(h->session);
        free(h);
        return COMM_ERR_INTERNAL;
    }

    *out = h;
    return COMM_OK;
}

comm_err_t comm_module_deinit(comm_module_handle_t handle)
{
    if (!handle) {
        return COMM_OK;
    }
    transport_deinit(handle->transport);
    session_deinit(handle->session);
    free(handle);
    return COMM_OK;
}

comm_err_t comm_module_run_once(comm_module_handle_t handle)
{
    if (!handle) {
        return COMM_ERR_INVALID_ARG;
    }
    return map_transport_error(transport_run_session(handle->transport));
}

comm_err_t comm_module_run_forever(comm_module_handle_t handle)
{
    if (!handle) {
        return COMM_ERR_INVALID_ARG;
    }
    for (;;) {
        comm_err_t err = comm_module_run_once(handle);
        if (err != COMM_OK && err != COMM_ERR_TIMEOUT) {
            return err;
        }
    }
}

bool comm_module_is_session_active(comm_module_handle_t handle)
{
    return handle && transport_is_session_active(handle->transport);
}

comm_err_t comm_module_force_abort(comm_module_handle_t handle)
{
    if (!handle) {
        return COMM_ERR_INVALID_ARG;
    }
    return map_transport_error(transport_abort(handle->transport));
}
