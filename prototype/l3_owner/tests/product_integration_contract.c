/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <dtv/l3_owner.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_PENDING 8

struct pending {
    int used;
    dtv_l3_provider_binding *binding;
    uint64_t id;
    dtv_l3_transfer_complete_fn complete;
    void *callback_context;
};

struct fixture {
    unsigned int claim_calls;
    unsigned int release_claim_calls;
    unsigned int submit_calls;
    unsigned int cancel_calls;
    unsigned int tune_calls;
    unsigned int tune_ex_calls;
    unsigned int attach_ex_calls;
    unsigned int stop_ex_calls;
    uint32_t seen_tune_system;
    uint32_t seen_tune_selection;
    uint32_t seen_tune_receiver;
    uint32_t seen_attach_receiver;
    uint32_t seen_attach_bridge;
    unsigned int bridge_start_calls;
    unsigned int bridge_stop_calls;
    unsigned int attach_calls;
    unsigned int stream_stop_calls;
    unsigned int factory_create_calls;
    unsigned int factory_release_calls;
    unsigned int bind_calls;
    unsigned int initialize_calls;
    int fail_initialize;
    int init_mode;
    int sync_complete;
    dtv_l3_result sync_result;
    int init_transfer_started;
    int init_transfer_done;
    dtv_l3_context *owner;
    unsigned int reader_open_calls;
    unsigned int reader_transmit_calls;
    unsigned int reader_reset_calls;
    unsigned int reader_close_calls;
    unsigned int lnb_restore_calls;
    int transmit_unknown;
    int lnb_inconsistent;
    int shutdown_transfer;
    int shutdown_started;
    dtv_l3_op *shutdown_op;
    atomic_ullong last_submit_id;
    uint32_t lnb_mode;
    dtv_l3_open_config seen_config;
    const uint8_t *seen_firmware;
    size_t seen_firmware_size;
    dtv_l3_ts_deliver_fn deliver;
    void *deliver_context;
    dtv_l3_product_adapter factory_adapter;
    int v1_manifest_mismatch;
    uint64_t seen_grant_token;
    uint16_t seen_grant_vendor;
    uint16_t seen_grant_product;
    dtv_l3_receiver_capability receivers[1];
    dtv_l3_reader_capability readers[1];
    dtv_l3_lnb_scope_capability lnb_scopes[1];
    dtv_l3_shared_scope_capability shared_scopes[1];
    dtv_l3_capability_catalog catalog;
    struct pending pending[MAX_PENDING];
};

static void sleep_ms(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
}

static struct pending *find_pending(struct fixture *fixture, uint64_t id) {
    unsigned int index;
    for (index = 0; index < MAX_PENDING; ++index) {
        if (fixture->pending[index].used &&
            fixture->pending[index].id == id) {
            return &fixture->pending[index];
        }
    }
    return NULL;
}

static struct pending *free_pending(struct fixture *fixture) {
    unsigned int index;
    for (index = 0; index < MAX_PENDING; ++index) {
        if (!fixture->pending[index].used) {
            return &fixture->pending[index];
        }
    }
    return NULL;
}

static dtv_l3_claim_result usb_claim(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->claim_calls;
    return (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

static dtv_l3_release_result usb_release_claim(void *context,
                                               dtv_l3_provider_binding *binding,
                                               uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->release_claim_calls;
    return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED,
                                   DTV_L3_CLAIM_NONE,
                                   DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
}

static dtv_l3_result usb_submit(void *context,
                                dtv_l3_provider_binding *binding,
                                uint64_t transfer_id,
                                const dtv_l3_transfer_request *request,
                                dtv_l3_transfer_complete_fn complete,
                                void *callback_context) {
    struct fixture *fixture = context;
    struct pending *slot;
    (void)request;
    ++fixture->submit_calls;
    atomic_store(&fixture->last_submit_id, transfer_id);
    if (fixture->sync_complete) {
        uint8_t response[2] = {0x5a, 0x5b};
        const dtv_l3_transfer_completion completion = {
            .result = fixture->sync_result,
            .data = fixture->sync_result == DTV_L3_OK ? response : NULL,
            .size = fixture->sync_result == DTV_L3_OK ? sizeof(response) : 0,
            .actual_length = fixture->sync_result == DTV_L3_OK ? sizeof(response) : 0,
            .actual_length_known = 1};
        complete(binding, transfer_id, &completion, callback_context);
        return DTV_L3_ACCEPTED;
    }
    slot = free_pending(fixture);
    if (slot == NULL) {
        return DTV_L3_BUSY;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->binding = binding;
    slot->id = transfer_id;
    slot->complete = complete;
    slot->callback_context = callback_context;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result usb_cancel(void *context,
                                dtv_l3_provider_binding *binding,
                                uint64_t transfer_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)transfer_id;
    ++fixture->cancel_calls;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result product_bind(void *context, dtv_l3_context *owner) {
    struct fixture *fixture = context;
    fixture->owner = owner;
    ++fixture->bind_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_initialize(void *context,
                                        dtv_l3_context *owner) {
    struct fixture *fixture = context;
    ++fixture->initialize_calls;
    if (fixture->fail_initialize) {
        return DTV_L3_FAILED;
    }
    if (fixture->init_mode != 0) {
        dtv_l3_transfer_request request;
        dtv_l3_op *op = NULL;
        dtv_l3_op_result result;
        memset(&request, 0, sizeof(request));
        request.kind = DTV_L3_TRANSFER_BULK;
        request.endpoint = 0x81;
        request.timeout_ms = 0;
        request.out_size = 0;
        request.in_capacity = 64;
        request.claim_index = 0;
        if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_ACCEPTED ||
            op == NULL) {
            return DTV_L3_FAILED;
        }
        fixture->init_transfer_started = 1;
        if (dtv_l3_op_wait(op, 2000) != DTV_L3_OK ||
            dtv_l3_op_poll(op, &result) != DTV_L3_OK) {
            (void)dtv_l3_op_release(op);
            return DTV_L3_FAILED;
        }
        const dtv_l3_result transfer_result = result.result;
        if (dtv_l3_op_release(op) != DTV_L3_OK) {
            return DTV_L3_FAILED;
        }
        fixture->init_transfer_done = 1;
        if (fixture->init_mode == 2 || transfer_result != DTV_L3_OK) {
            return DTV_L3_FAILED;
        }
    }
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_status(
    void *context,
    uint32_t reader_id,
    dtv_l3_reader_observation *out) {
    (void)context;
    (void)reader_id;
    out->presence = DTV_L3_CARD_PRESENCE_PRESENT;
    out->presence_generation = 1;
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_open(void *context,
                                         uint32_t reader_id,
                                         uint64_t deadline_ns,
                                         void **session_out,
                                         uint8_t *atr,
                                         size_t atr_capacity,
                                         size_t *atr_length) {
    struct fixture *fixture = context;
    (void)reader_id;
    (void)deadline_ns;
    ++fixture->reader_open_calls;
    *session_out = fixture;
    if (atr != NULL && atr_capacity >= 2) {
        atr[0] = 0x3b;
        atr[1] = 0x00;
        *atr_length = 2;
    } else {
        *atr_length = 0;
    }
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_transmit(
    void *context,
    void *session,
    const uint8_t *apdu,
    size_t apdu_length,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_length,
    uint64_t deadline_ns) {
    struct fixture *fixture = context;
    (void)session;
    (void)apdu;
    (void)apdu_length;
    (void)deadline_ns;
    ++fixture->reader_transmit_calls;
    if (fixture->transmit_unknown) {
        fixture->transmit_unknown = 0;
        return DTV_L3_OUTCOME_UNKNOWN;
    }
    if (response != NULL && response_capacity >= 2) {
        response[0] = 0x90;
        response[1] = 0x00;
        *response_length = 2;
    } else {
        *response_length = 0;
    }
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_reset(void *context,
                                          void *session,
                                          uint64_t deadline_ns,
                                          uint8_t *atr,
                                          size_t atr_capacity,
                                          size_t *atr_length) {
    struct fixture *fixture = context;
    (void)session;
    (void)deadline_ns;
    ++fixture->reader_reset_calls;
    if (atr != NULL && atr_capacity >= 1) {
        atr[0] = 0x3b;
        *atr_length = 1;
    }
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_close(void *context,
                                          void *session,
                                          uint32_t disposition,
                                          uint64_t deadline_ns) {
    struct fixture *fixture = context;
    (void)session;
    (void)disposition;
    (void)deadline_ns;
    ++fixture->reader_close_calls;
    if (fixture->shutdown_transfer && !fixture->shutdown_started &&
        fixture->owner != NULL) {
        fixture->shutdown_started = 1;
        dtv_l3_transfer_request request;
        dtv_l3_op_result result;
        memset(&request, 0, sizeof(request));
        request.kind = DTV_L3_TRANSFER_BULK;
        request.endpoint = 0x81;
        request.timeout_ms = 0;
        request.in_capacity = 64;
        request.claim_index = 0;
        if (dtv_l3_transfer_start(fixture->owner, &request,
                                  &fixture->shutdown_op) != DTV_L3_ACCEPTED) {
            return DTV_L3_FAILED;
        }
        /* Intentionally time out and keep the accepted op in flight. */
        (void)dtv_l3_op_wait(fixture->shutdown_op, 20);
        (void)dtv_l3_op_poll(fixture->shutdown_op, &result);
    }
    return DTV_L3_OK;
}

static dtv_l3_result product_lnb_status(void *context,
                                        uint32_t scope_id,
                                        dtv_l3_lnb_state *out) {
    struct fixture *fixture = context;
    (void)scope_id;
    out->controlled = fixture->lnb_inconsistent ? DTV_L3_LNB_ON_15V
                                                : DTV_L3_LNB_OFF;
    out->consistency = fixture->lnb_inconsistent
                           ? DTV_L3_LNB_CONSISTENCY_INCONSISTENT
                           : DTV_L3_LNB_CONSISTENCY_CONSISTENT;
    out->readback = DTV_L3_SUPPORT_SUPPORTED;
    out->scope_generation = 1;
    return DTV_L3_OK;
}

static dtv_l3_result product_lnb_set(void *context,
                                     uint32_t scope_id,
                                     dtv_l3_lnb_command command,
                                     uint64_t deadline_ns,
                                     dtv_l3_lnb_state *out) {
    struct fixture *fixture = context;
    (void)scope_id;
    (void)deadline_ns;
    if (fixture->lnb_inconsistent) {
        out->controlled = DTV_L3_LNB_ON_15V;
        out->consistency = DTV_L3_LNB_CONSISTENCY_INCONSISTENT;
    } else {
        out->controlled = command;
        out->consistency = DTV_L3_LNB_CONSISTENCY_CONSISTENT;
    }
    out->readback = DTV_L3_SUPPORT_SUPPORTED;
    out->scope_generation = 1;
    return DTV_L3_OK;
}

static dtv_l3_result product_lnb_restore(void *context,
                                         uint32_t scope_id,
                                         dtv_l3_restore_result *out) {
    struct fixture *fixture = context;
    (void)scope_id;
    ++fixture->lnb_restore_calls;
    *out = DTV_L3_RESTORE_SUCCEEDED;
    return DTV_L3_OK;
}

static dtv_l3_result product_scope(void *context,
                                   uint32_t receiver_id,
                                   dtv_l3_scope_info *scope) {
    (void)context;
    (void)receiver_id;
    scope->bridge_id = 3;
    scope->compatibility = DTV_L3_LEASE_SHAREABLE;
    return DTV_L3_OK;
}

static dtv_l3_result product_tune(void *context,
                                  dtv_l3_provider_binding *binding,
                                  uint32_t receiver_id,
                                  uint64_t requested_hz,
                                  uint64_t *programmed_hz) {
    struct fixture *fixture = context;
    (void)binding;
    (void)receiver_id;
    ++fixture->tune_calls;
    *programmed_hz = requested_hz;
    return DTV_L3_OK;
}

static dtv_l3_result product_tune_ex(void *context,
                                   dtv_l3_provider_binding *binding,
                                   uint32_t receiver_id,
                                   const dtv_l3_tune_request *request,
                                   uint64_t *programmed_hz) {
    struct fixture *fixture = context;
    (void)binding;
    ++fixture->tune_ex_calls;
    fixture->seen_tune_system = request->system;
    fixture->seen_tune_selection = request->selection;
    fixture->seen_tune_receiver = receiver_id;
    *programmed_hz = request->requested_hz;
    return DTV_L3_OK;
}

static dtv_l3_result product_attach_ex(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint64_t stream_id,
                                     uint32_t receiver_id,
                                     uint32_t bridge_id,
                                     dtv_l3_ts_deliver_fn deliver,
                                     void *callback_context) {
    struct fixture *fixture = context;
    (void)binding;
    (void)stream_id;
    ++fixture->attach_ex_calls;
    fixture->seen_attach_receiver = receiver_id;
    fixture->seen_attach_bridge = bridge_id;
    fixture->deliver = deliver;
    fixture->deliver_context = callback_context;
    return DTV_L3_OK;
}

static dtv_l3_result product_stop_ex(void *context,
                                   dtv_l3_provider_binding *binding,
                                   uint64_t stream_id,
                                   uint32_t receiver_id,
                                   uint32_t bridge_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)stream_id;
    (void)receiver_id;
    (void)bridge_id;
    ++fixture->stop_ex_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_bridge_start(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t bridge_id,
                                          uint32_t receiver_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)bridge_id;
    (void)receiver_id;
    ++fixture->bridge_start_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_bridge_stop(void *context,
                                         dtv_l3_provider_binding *binding,
                                         uint32_t bridge_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)bridge_id;
    ++fixture->bridge_stop_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_attach(void *context,
                                    dtv_l3_provider_binding *binding,
                                    uint64_t stream_id,
                                    dtv_l3_ts_deliver_fn deliver,
                                    void *callback_context) {
    struct fixture *fixture = context;
    (void)binding;
    (void)stream_id;
    ++fixture->attach_calls;
    fixture->deliver = deliver;
    fixture->deliver_context = callback_context;
    return DTV_L3_OK;
}

static dtv_l3_result product_stream_stop(void *context,
                                         dtv_l3_provider_binding *binding,
                                         uint64_t stream_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)stream_id;
    ++fixture->stream_stop_calls;
    return DTV_L3_OK;
}

static dtv_l3_usb_provider usb_provider_for(struct fixture *fixture) {
    return (dtv_l3_usb_provider){
        .provider_context = fixture,
        .claim = usb_claim,
        .release_claim = usb_release_claim,
        .submit = usb_submit,
        .cancel = usb_cancel,
        .cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE};
}

static dtv_l3_product_adapter adapter_for(struct fixture *fixture,
                                          const dtv_l3_firmware *firmware) {
    return (dtv_l3_product_adapter){
        .product_context = fixture,
        .capabilities = {.receiver_count = 2,
                         .reader_count = 1,
                         .lnb_power = 1,
                         .serial = 0,
                         .cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE},
        .firmware = firmware,
        .bind = product_bind,
        .initialize = product_initialize,
        .scope_query = product_scope,
        .tune = product_tune,
        .tune_ex = product_tune_ex,
        .bridge_start = product_bridge_start,
        .bridge_stop = product_bridge_stop,
        .stream_attach = product_attach,
        .stream_attach_ex = product_attach_ex,
        .stream_stop = product_stream_stop,
        .stream_stop_ex = product_stop_ex,
        .reader_status = product_reader_status,
        .reader_open = product_reader_open,
        .reader_transmit = product_reader_transmit,
        .reader_reset = product_reader_reset,
        .reader_close = product_reader_close,
        .lnb_status = product_lnb_status,
        .lnb_set = product_lnb_set,
        .lnb_restore = product_lnb_restore};
}

static int wait_op_terminal(dtv_l3_op *op, dtv_l3_op_result *result) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (dtv_l3_op_poll(op, result) != DTV_L3_OK) {
            return 0;
        }
        if (result->state == DTV_L3_OP_DONE ||
            result->state == DTV_L3_OP_FAILED) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

static int open_ready_ex(dtv_l3_usb_provider *usb,
                         dtv_l3_product_adapter *adapter,
                         dtv_l3_context **owner_out) {
    dtv_l3_open_result opened = dtv_l3_open_ex(usb, adapter, 1);
    dtv_l3_op_result result;
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 0;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 0;
    }
    *owner_out = opened.owner;
    return 1;
}

static int wait_releasable(dtv_l3_context *owner) {
    unsigned int attempt;
    dtv_l3_close_status status;
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (dtv_l3_close_status_get(owner, &status) != DTV_L3_OK) {
            return 0;
        }
        if (status.releasable) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

static int release_owner(dtv_l3_context *owner) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4000; ++attempt) {
        const dtv_l3_result result = dtv_l3_context_release(owner);
        if (result != DTV_L3_BUSY) {
            return result == DTV_L3_OK;
        }
        sleep_ms(1);
    }
    return 0;
}

static int close_and_release(dtv_l3_context *owner) {
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner)) {
        return 0;
    }
    return release_owner(owner);
}

static int fire_pending(struct fixture *fixture, uint64_t id) {
    struct pending slot;
    struct pending *found = find_pending(fixture, id);
    uint8_t data[2] = {0xaa, 0xbb};
    if (found == NULL) {
        return 0;
    }
    slot = *found;
    memset(found, 0, sizeof(*found));
    const dtv_l3_transfer_completion completion = {
        .result = DTV_L3_OK,
        .data = data,
        .size = sizeof(data),
        .actual_length = sizeof(data),
        .actual_length_known = 1};
    slot.complete(slot.binding, slot.id, &completion, slot.callback_context);
    return 1;
}

static uint64_t only_pending_id(struct fixture *fixture) {
    unsigned int index;
    for (index = 0; index < MAX_PENDING; ++index) {
        if (fixture->pending[index].used) {
            return fixture->pending[index].id;
        }
    }
    return 0;
}

static int exercise_product(dtv_l3_context *owner, struct fixture *fixture) {
    dtv_l3_transfer_request request;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result op_result;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_lease_status lease_status;
    dtv_l3_stream_status stream_status;
    dtv_l3_reader *reader = NULL;
    uint64_t programmed = 0;
    uint64_t revision = 0;
    uint64_t id;
    uint8_t ts_data[376];
    uint8_t out[188];
    size_t bytes = 0;
    dtv_l3_read_state read_state;

    memset(&request, 0, sizeof(request));
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81;
    request.timeout_ms = 0;
    request.in_capacity = 64;
    request.claim_index = 0;
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == NULL) {
        return 10;
    }
    id = only_pending_id(fixture);
    if (!fire_pending(fixture, id) || !wait_op_terminal(op, &op_result) ||
        op_result.state != DTV_L3_OP_DONE || op_result.data_size != 2 ||
        dtv_l3_op_release(op) != DTV_L3_OK) {
        return 11;
    }
    dtv_l3_tune_request tune_request = {
        .system = DTV_L3_TUNE_SYSTEM_ISDB_T,
        .selection = DTV_L3_TUNE_SELECTION_AUTO,
        .requested_hz = 500000000};
    if (dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_OK ||
        dtv_l3_lease_tune_ex(lease, &tune_request, &programmed, &revision) !=
            DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK ||
        stream == NULL || fixture->deliver == NULL) {
        return 12;
    }
    memset(ts_data, 0x47, sizeof(ts_data));
    fixture->deliver(fixture->deliver_context, ts_data, sizeof(ts_data));
    if (dtv_l3_stream_read_ts(stream, out, sizeof(out), &bytes,
                              &read_state) != DTV_L3_OK ||
        read_state != DTV_L3_READ_DATA || bytes != sizeof(out)) {
        return 13;
    }
    if (dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK ||
        dtv_l3_lease_status_get(lease, &lease_status) != DTV_L3_OK) {
        return 14;
    }
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
            DTV_L3_OK ||
        dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 15;
    }
    if (dtv_l3_reader_acquire(owner, 0, &reader) != DTV_L3_UNSUPPORTED ||
        dtv_l3_lnb_set(owner, 0, DTV_L3_LNB_ON_15V, 0, NULL) !=
            DTV_L3_REJECTED) {
        return 16;
    }
    if (fixture->tune_ex_calls != 1 || fixture->bridge_start_calls != 1 ||
        fixture->attach_ex_calls != 1 || fixture->stop_ex_calls != 1 ||
        fixture->seen_tune_system != DTV_L3_TUNE_SYSTEM_ISDB_T ||
        fixture->seen_tune_selection != DTV_L3_TUNE_SELECTION_AUTO ||
        fixture->seen_tune_receiver != 0 ||
        fixture->seen_attach_receiver != 0 ||
        fixture->seen_attach_bridge != 3) {
        return 18;
    }
    return 0;
}

static int test_separated_open(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter;
    dtv_l3_capabilities capabilities;
    dtv_l3_context *owner = NULL;
    const uint8_t firmware[3] = {0x01, 0x02, 0x03};
    const dtv_l3_firmware firmware_view = {
        .source = DTV_L3_FIRMWARE_BYTES,
        .bytes = firmware,
        .size = sizeof(firmware)};
    int rc;

    memset(&fixture, 0, sizeof(fixture));
    usb = usb_provider_for(&fixture);
    adapter = adapter_for(&fixture, &firmware_view);
    if (!open_ready_ex(&usb, &adapter, &owner)) {
        return 1;
    }
    if (fixture.bind_calls != 1 || fixture.initialize_calls != 1) {
        return 21;
    }
    if (dtv_l3_capabilities_get(owner, &capabilities) != DTV_L3_OK ||
        capabilities.receiver_count != 2 || capabilities.reader_count != 1 ||
        capabilities.lnb_power != 1 || capabilities.serial != 0 ||
        capabilities.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE) {
        return 2;
    }
    rc = exercise_product(owner, &fixture);
    if (rc != 0) {
        return rc;
    }
    if (!close_and_release(owner)) {
        return 19;
    }
    if (fixture.release_claim_calls != 1 || fixture.cancel_calls != 0) {
        return 20;
    }
    return 0;
}

static dtv_l3_result factory_create_ok(void *context,
                                       const dtv_l3_open_config *config,
                                       const dtv_l3_product_adapter **out,
                                       void **owner_out) {
    struct fixture *fixture = context;
    ++fixture->factory_create_calls;
    if (config != NULL) {
        fixture->seen_config = *config;
        if (config->firmware != NULL &&
            config->firmware->source == DTV_L3_FIRMWARE_BYTES) {
            fixture->seen_firmware = config->firmware->bytes;
            fixture->seen_firmware_size = config->firmware->size;
        }
    }
    *out = &fixture->factory_adapter;
    *owner_out = fixture;
    return DTV_L3_OK;
}

static dtv_l3_result factory_create_fail(void *context,
                                         const dtv_l3_open_config *config,
                                         const dtv_l3_product_adapter **out,
                                         void **owner_out) {
    struct fixture *fixture = context;
    (void)config;
    ++fixture->factory_create_calls;
    *out = NULL;
    *owner_out = fixture;
    return DTV_L3_FAILED;
}

static void factory_release(void *owner) {
    struct fixture *fixture = owner;
    ++fixture->factory_release_calls;
}

static dtv_l3_result factory_create_v1(
    void *context,
    const dtv_l3_open_args *args,
    const dtv_l3_product_adapter **out,
    uint32_t *claim_count_out,
    void **owner_out) {
    struct fixture *fixture = context;
    ++fixture->factory_create_calls;
    if (args->grant_count != 0 && args->grants != NULL &&
        args->grants[0].descriptor != NULL) {
        fixture->seen_grant_token = args->grants[0].token;
        fixture->seen_grant_vendor = args->grants[0].descriptor->vendor_id;
        fixture->seen_grant_product = args->grants[0].descriptor->product_id;
    }
    if (args->firmware != NULL && args->firmware->manifest != NULL &&
        (fixture->v1_manifest_mismatch ||
         args->firmware->manifest->length_bytes != args->firmware->size)) {
        *out = NULL;
        *owner_out = fixture;
        return DTV_L3_FAILED;
    }
    *out = &fixture->factory_adapter;
    *claim_count_out = 2;
    *owner_out = fixture;
    return DTV_L3_OK;
}

static dtv_l3_factory factory_v1_for(struct fixture *fixture) {
    return (dtv_l3_factory){.struct_size = sizeof(dtv_l3_factory),
                            .factory_context = fixture,
                            .create = factory_create_v1,
                            .release = factory_release};
}

static void fixture_catalog_init(struct fixture *fixture,
                                 const dtv_l3_firmware *firmware) {
    fixture->receivers[0] = (dtv_l3_receiver_capability){
        .id = 0,
        .systems = DTV_L3_SYSTEM_ISDB_T,
        .pid_filter = DTV_L3_PID_FILTER_SOFTWARE,
        .support = DTV_L3_SUPPORT_SUPPORTED};
    fixture->readers[0] = (dtv_l3_reader_capability){
        .id = 0,
        .external_apdu = DTV_L3_SUPPORT_UNSUPPORTED,
        .support = DTV_L3_SUPPORT_SUPPORTED};
    fixture->lnb_scopes[0] = (dtv_l3_lnb_scope_capability){
        .id = 0, .support = DTV_L3_SUPPORT_UNSUPPORTED};
    fixture->shared_scopes[0] = (dtv_l3_shared_scope_capability){
        .id = 3,
        .compatibility = DTV_L3_LEASE_SHAREABLE,
        .support = DTV_L3_SUPPORT_SUPPORTED};
    fixture->catalog = (dtv_l3_capability_catalog){
        .receivers = fixture->receivers,
        .receiver_count = 1,
        .readers = fixture->readers,
        .reader_count = 1,
        .lnb_scopes = fixture->lnb_scopes,
        .lnb_scope_count = 1,
        .shared_scopes = fixture->shared_scopes,
        .shared_scope_count = 1};
    fixture->factory_adapter = adapter_for(fixture, firmware);
    fixture->factory_adapter.catalog = &fixture->catalog;
}

static dtv_l3_adapter_factory factory_for(struct fixture *fixture) {
    return (dtv_l3_adapter_factory){.factory_context = fixture,
                                    .create = factory_create_ok,
                                    .release = factory_release};
}

static int test_factory_open(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_adapter_factory factory;
    dtv_l3_open_config config;
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    const uint8_t firmware[2] = {0x10, 0x20};
    const dtv_l3_firmware firmware_view = {
        .source = DTV_L3_FIRMWARE_BYTES,
        .bytes = firmware,
        .size = sizeof(firmware)};
    int rc;

    memset(&fixture, 0, sizeof(fixture));
    fixture.factory_adapter = adapter_for(&fixture, &firmware_view);
    usb = usb_provider_for(&fixture);
    factory = factory_for(&fixture);
    memset(&config, 0, sizeof(config));
    config.model = 7;
    config.selector = 0x1234;
    config.firmware = &firmware_view;
    opened = dtv_l3_open_factory(&usb, &factory, &config, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (fixture.factory_create_calls != 1 ||
        fixture.seen_config.model != 7 ||
        fixture.seen_config.selector != 0x1234 ||
        fixture.seen_firmware != firmware ||
        fixture.seen_firmware_size != sizeof(firmware)) {
        return 2;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 3;
    }
    rc = exercise_product(opened.owner, &fixture);
    if (rc != 0) {
        return rc;
    }
    if (!close_and_release(opened.owner)) {
        return 4;
    }
    if (fixture.factory_release_calls != 1) {
        return 5;
    }
    return 0;
}

static int test_factory_failure(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_adapter_factory factory;
    dtv_l3_open_config config;
    dtv_l3_open_result opened;

    memset(&fixture, 0, sizeof(fixture));
    usb = usb_provider_for(&fixture);
    factory = factory_for(&fixture);
    factory.create = factory_create_fail;
    memset(&config, 0, sizeof(config));
    opened = dtv_l3_open_factory(&usb, &factory, &config, 1);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL || fixture.factory_create_calls != 1 ||
        fixture.factory_release_calls != 1 || fixture.claim_calls != 0) {
        return 1;
    }
    return 0;
}

static int test_args_factory(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_factory factory;
    dtv_l3_open_args args;
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    dtv_l3_grant_descriptor descriptor;
    dtv_l3_grant grant;
    const dtv_l3_capability_catalog *catalog = NULL;
    const uint8_t firmware[2] = {0x55, 0xaa};
    const dtv_l3_firmware_manifest manifest = {
        .format = 0x46523330u, .length_bytes = sizeof(firmware)};
    const dtv_l3_firmware firmware_view = {
        .source = DTV_L3_FIRMWARE_BYTES,
        .manifest = &manifest,
        .bytes = firmware,
        .size = sizeof(firmware)};

    memset(&fixture, 0, sizeof(fixture));
    fixture_catalog_init(&fixture, &firmware_view);
    usb = usb_provider_for(&fixture);
    factory = factory_v1_for(&fixture);
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.vendor_id = 0x054c;
    descriptor.product_id = 0x04b2;
    descriptor.bus = 5;
    descriptor.address = 4;
    descriptor.num_interfaces = 1;
    grant.token = 0xabc;
    grant.descriptor = &descriptor;
    memset(&args, 0, sizeof(args));
    args.struct_size = sizeof(args);
    args.usb_provider = &usb;
    args.factory = &factory;
    args.grants = &grant;
    args.grant_count = 1;
    args.firmware = &firmware_view;
    opened = dtv_l3_open_with_args(&args);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (fixture.factory_create_calls != 1 ||
        fixture.seen_grant_token != 0xabc ||
        fixture.seen_grant_vendor != 0x054c ||
        fixture.seen_grant_product != 0x04b2) {
        return 2;
    }
    if (dtv_l3_capabilities_get_catalog(opened.owner, &catalog) != DTV_L3_OK ||
        catalog == NULL || catalog->receiver_count != 1 ||
        catalog->receivers[0].systems != DTV_L3_SYSTEM_ISDB_T ||
        catalog->receivers[0].pid_filter != DTV_L3_PID_FILTER_SOFTWARE ||
        catalog->readers[0].external_apdu != DTV_L3_SUPPORT_UNSUPPORTED ||
        catalog->lnb_scopes[0].support != DTV_L3_SUPPORT_UNSUPPORTED ||
        catalog->shared_scopes[0].support != DTV_L3_SUPPORT_SUPPORTED) {
        return 3;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK || fixture.claim_calls != 2 ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 4;
    }
    if (!close_and_release(opened.owner) ||
        fixture.factory_release_calls != 1) {
        return 5;
    }
    return 0;
}

static int test_args_factory_manifest_reject(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_factory factory;
    dtv_l3_open_args args;
    dtv_l3_open_result opened;
    const uint8_t firmware[2] = {0x01, 0x02};
    const dtv_l3_firmware_manifest manifest = {
        .format = 0x46523330u, .length_bytes = 99};
    const dtv_l3_firmware firmware_view = {
        .source = DTV_L3_FIRMWARE_BYTES,
        .manifest = &manifest,
        .bytes = firmware,
        .size = sizeof(firmware)};

    memset(&fixture, 0, sizeof(fixture));
    fixture.v1_manifest_mismatch = 1;
    fixture_catalog_init(&fixture, &firmware_view);
    usb = usb_provider_for(&fixture);
    factory = factory_v1_for(&fixture);
    memset(&args, 0, sizeof(args));
    args.struct_size = sizeof(args);
    args.usb_provider = &usb;
    args.factory = &factory;
    args.firmware = &firmware_view;
    opened = dtv_l3_open_with_args(&args);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL || fixture.claim_calls != 0 ||
        fixture.factory_release_calls != 1) {
        return 1;
    }
    return 0;
}

static int test_initialize_transfer_ok(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter;
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    dtv_l3_open_status status;

    memset(&fixture, 0, sizeof(fixture));
    fixture.init_mode = 1;
    fixture.sync_complete = 1;
    fixture.sync_result = DTV_L3_OK;
    usb = usb_provider_for(&fixture);
    adapter = adapter_for(&fixture, NULL);
    opened = dtv_l3_open_ex(&usb, &adapter, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK || fixture.init_transfer_started != 1 ||
        fixture.init_transfer_done != 1 || fixture.submit_calls != 1) {
        return 2;
    }
    if (dtv_l3_open_status_get(opened.owner, &status) != DTV_L3_OK ||
        status.state != DTV_L3_OPEN_READY) {
        return 3;
    }
    if (dtv_l3_op_release(opened.op) != DTV_L3_OK ||
        !close_and_release(opened.owner)) {
        return 4;
    }
    return 0;
}

static int test_initialize_transfer_failure(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter;
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    dtv_l3_open_status status;

    memset(&fixture, 0, sizeof(fixture));
    fixture.init_mode = 2;
    fixture.sync_complete = 1;
    fixture.sync_result = DTV_L3_OK;
    usb = usb_provider_for(&fixture);
    adapter = adapter_for(&fixture, NULL);
    adapter.reader_close = NULL;
    adapter.lnb_restore = NULL;
    opened = dtv_l3_open_ex(&usb, &adapter, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED || result.result != DTV_L3_FAILED ||
        fixture.init_transfer_started != 1 ||
        fixture.init_transfer_done != 1) {
        return 2;
    }
    if (dtv_l3_open_status_get(opened.owner, &status) != DTV_L3_OK ||
        status.state != DTV_L3_OPEN_FAILED) {
        return 3;
    }
    if (dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 4;
    }
    if (dtv_l3_close(opened.owner, 2000) != DTV_L3_OK ||
        !wait_releasable(opened.owner) ||
        fixture.release_claim_calls != 1) {
        return 5;
    }
    if (!release_owner(opened.owner)) {
        return 6;
    }
    return 0;
}

static int test_initialize_failure(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter;
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    dtv_l3_open_status status;

    memset(&fixture, 0, sizeof(fixture));
    fixture.fail_initialize = 1;
    usb = usb_provider_for(&fixture);
    adapter = adapter_for(&fixture, NULL);
    opened = dtv_l3_open_ex(&usb, &adapter, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED || result.result != DTV_L3_FAILED) {
        return 2;
    }
    if (dtv_l3_open_status_get(opened.owner, &status) != DTV_L3_OK ||
        status.state != DTV_L3_OPEN_FAILED || fixture.initialize_calls != 1 ||
        fixture.bind_calls != 1) {
        return 3;
    }
    if (dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 4;
    }
    if (dtv_l3_close(opened.owner, 2000) != DTV_L3_OK ||
        !wait_releasable(opened.owner) ||
        fixture.release_claim_calls != 1) {
        return 5;
    }
    if (!release_owner(opened.owner)) {
        return 6;
    }
    return 0;
}

static uint64_t now_ns(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static int read_packets(dtv_l3_stream *stream, uint8_t *buffer, size_t count) {
    size_t offset = 0;
    while (offset < count) {
        size_t bytes = 0;
        dtv_l3_read_state state;
        if (dtv_l3_stream_read_ts(stream, buffer + offset * 188,
                                  (count - offset) * 188, &bytes,
                                  &state) != DTV_L3_OK ||
            state != DTV_L3_READ_DATA || bytes == 0) {
            return 0;
        }
        offset += bytes / 188;
    }
    return 1;
}

static int test_queue_capacity(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter;
    dtv_l3_context *owner = NULL;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_stream_status status;
    uint64_t programmed = 0;
    uint64_t revision = 0;
    uint8_t packets[20 * 188];
    uint8_t readback[348 * 188];
    uint8_t small[21 * 188];
    uint8_t large[348 * 188];
    uint64_t small_start;
    uint64_t small_elapsed;
    uint64_t large_start;
    uint64_t large_elapsed;
    unsigned int index;

    memset(&fixture, 0, sizeof(fixture));
    usb = usb_provider_for(&fixture);
    adapter = adapter_for(&fixture, NULL);
    if (!open_ready_ex(&usb, &adapter, &owner)) {
        return 1;
    }
    if (dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_OK ||
        dtv_l3_lease_tune(lease, 500000000, &programmed, &revision) !=
            DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK || stream == NULL ||
        fixture.deliver == NULL) {
        return 2;
    }
    for (index = 0; index < sizeof(packets); ++index) {
        packets[index] = (uint8_t)(index / 188);
    }
    fixture.deliver(fixture.deliver_context, packets, sizeof(packets));
    if (dtv_l3_stream_status_get(stream, &status) != DTV_L3_OK ||
        status.queued_packets != 20 || status.dropped_overflow_packets != 0) {
        return 3;
    }
    if (!read_packets(stream, readback, 20) ||
        memcmp(readback, packets, sizeof(packets)) != 0) {
        return 4;
    }
    memset(small, 0x31, sizeof(small));
    small_start = now_ns();
    fixture.deliver(fixture.deliver_context, small, sizeof(small));
    small_elapsed = now_ns() - small_start;
    if (!read_packets(stream, readback, 21)) {
        return 5;
    }
    memset(large, 0x32, sizeof(large));
    large_start = now_ns();
    fixture.deliver(fixture.deliver_context, large, sizeof(large));
    large_elapsed = now_ns() - large_start;
    if (dtv_l3_stream_status_get(stream, &status) != DTV_L3_OK ||
        status.queued_packets != 348 ||
        status.dropped_overflow_packets != 0) {
        return 6;
    }
    if (!read_packets(stream, readback, 20)) {
        return 7;
    }
    /* 64 KiB must not show per-packet quadratic staging work. */
    if (large_elapsed > small_elapsed * 100 + 10000000ull) {
        return 8;
    }
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
            DTV_L3_OK ||
        dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 9;
    }
    return close_and_release(owner) ? 0 : 10;
}

static int test_g8_reader_lnb_shutdown(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_factory factory;
    dtv_l3_open_args args;
    dtv_l3_open_result opened;
    dtv_l3_op_result op_result;
    dtv_l3_card_info card;
    dtv_l3_reader *reader = NULL;
    dtv_l3_reader *second = NULL;
    dtv_l3_lnb_state lnb;
    uint8_t atr[8];
    size_t atr_length = 0;
    const uint8_t apdu[4] = {0x00, 0xa4, 0x00, 0x00};
    uint8_t response[8];
    size_t response_length = 0;
    unsigned int attempt;

    memset(&fixture, 0, sizeof(fixture));
    fixture_catalog_init(&fixture, NULL);
    usb = usb_provider_for(&fixture);
    factory = factory_v1_for(&fixture);
    memset(&args, 0, sizeof(args));
    args.struct_size = sizeof(args);
    args.usb_provider = &usb;
    args.factory = &factory;
    args.power_permission = 1;
    opened = dtv_l3_open_with_args(&args);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &op_result) != DTV_L3_OK ||
        op_result.result != DTV_L3_OK || dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 2;
    }
    if (dtv_l3_card_status(opened.owner, 0, &card) != DTV_L3_OK ||
        card.presence != DTV_L3_CARD_PRESENCE_PRESENT) {
        return 3;
    }
    if (dtv_l3_reader_open(opened.owner, 0, 0, &reader, atr, sizeof(atr),
                           &atr_length) != DTV_L3_OK ||
        reader == NULL || atr_length != 2) {
        return 4;
    }
    if (dtv_l3_reader_open(opened.owner, 0, 0, &second, atr, sizeof(atr),
                           &atr_length) != DTV_L3_BUSY ||
        second != NULL) {
        return 5;
    }
    if (dtv_l3_reader_transmit(reader, 1, apdu, sizeof(apdu), response,
                               sizeof(response), &response_length, 0) !=
            DTV_L3_OK ||
        response_length != 2 ||
        dtv_l3_reader_transmit(reader, 1, apdu, sizeof(apdu), response,
                               sizeof(response), &response_length, 0) !=
            DTV_L3_INVALID) {
        return 6;
    }
    fixture.transmit_unknown = 1;
    if (dtv_l3_reader_transmit(reader, 2, apdu, sizeof(apdu), response,
                               sizeof(response), &response_length, 0) !=
        DTV_L3_OUTCOME_UNKNOWN) {
        return 7;
    }
    if (dtv_l3_reader_transmit(reader, 3, apdu, sizeof(apdu), response,
                               sizeof(response), &response_length, 0) !=
            DTV_L3_BUSY ||
        dtv_l3_reader_reset(reader, 0, atr, sizeof(atr), &atr_length) !=
            DTV_L3_OK) {
        return 8;
    }
    if (dtv_l3_lnb_set(opened.owner, 0, DTV_L3_LNB_ON_15V, 0, &lnb) !=
            DTV_L3_OK ||
        lnb.controlled != DTV_L3_LNB_ON_15V) {
        return 9;
    }
    fixture.lnb_inconsistent = 1;
    if (dtv_l3_lnb_set(opened.owner, 0, DTV_L3_LNB_OFF, 0, &lnb) != DTV_L3_OK ||
        lnb.consistency != DTV_L3_LNB_CONSISTENCY_INCONSISTENT ||
        dtv_l3_lnb_status(opened.owner, 0, &lnb) != DTV_L3_OK ||
        lnb.consistency != DTV_L3_LNB_CONSISTENCY_INCONSISTENT) {
        return 10;
    }
    fixture.shutdown_transfer = 1;
    if (dtv_l3_close(opened.owner, 0) != DTV_L3_TIMEOUT) {
        return 11;
    }
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (fixture.reader_close_calls != 0 &&
            fixture.lnb_restore_calls != 0) {
            break;
        }
        sleep_ms(1);
    }
    if (fixture.reader_close_calls == 0 || fixture.lnb_restore_calls != 1 ||
        fixture.release_claim_calls != 0 || fixture.shutdown_op == NULL) {
        return 12;
    }
    fire_pending(&fixture, atomic_load(&fixture.last_submit_id));
    if (dtv_l3_op_release(fixture.shutdown_op) != DTV_L3_OK) {
        return 13;
    }
    fixture.shutdown_op = NULL;
    if (dtv_l3_reader_release(reader, DTV_L3_CARD_DISPOSITION_LEAVE, 0) !=
            DTV_L3_OK ||
        fixture.reader_close_calls < 2) {
        return 14;
    }
    if (!wait_releasable(opened.owner)) {
        return 15;
    }
    if (!release_owner(opened.owner)) {
        return 16;
    }
    return 0;
}

static int test_missing_product_callbacks(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter;
    dtv_l3_context *owner = NULL;
    dtv_l3_lease *lease = NULL;

    memset(&fixture, 0, sizeof(fixture));
    usb = usb_provider_for(&fixture);
    memset(&adapter, 0, sizeof(adapter));
    adapter.product_context = &fixture;
    adapter.capabilities.receiver_count = 1;
    if (!open_ready_ex(&usb, &adapter, &owner)) {
        return 1;
    }
    if (dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_UNSUPPORTED ||
        lease != NULL) {
        return 2;
    }
    if (dtv_l3_reader_acquire(owner, 0, NULL) != DTV_L3_INVALID) {
        return 3;
    }
    if (!close_and_release(owner)) {
        return 4;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 64;
    }
    if (strcmp(argv[1], "separated") == 0) {
        return test_separated_open();
    }
    if (strcmp(argv[1], "factory") == 0) {
        return test_factory_open();
    }
    if (strcmp(argv[1], "factory-fail") == 0) {
        return test_factory_failure();
    }
    if (strcmp(argv[1], "missing") == 0) {
        return test_missing_product_callbacks();
    }
    if (strcmp(argv[1], "args-factory") == 0) {
        return test_args_factory();
    }
    if (strcmp(argv[1], "args-manifest-reject") == 0) {
        return test_args_factory_manifest_reject();
    }
    if (strcmp(argv[1], "init-fail") == 0) {
        return test_initialize_failure();
    }
    if (strcmp(argv[1], "init-transfer-ok") == 0) {
        return test_initialize_transfer_ok();
    }
    if (strcmp(argv[1], "init-transfer-fail") == 0) {
        return test_initialize_transfer_failure();
    }
    if (strcmp(argv[1], "capacity") == 0) {
        return test_queue_capacity();
    }
    if (strcmp(argv[1], "g8") == 0) {
        return test_g8_reader_lnb_shutdown();
    }
    return 65;
}
