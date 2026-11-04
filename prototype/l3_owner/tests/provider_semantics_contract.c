/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <dtv/l3_owner.h>

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_PENDING 16
#define MAX_PAYLOAD 64

struct pending {
    int used;
    dtv_l3_provider_binding *binding;
    uint64_t id;
    uint32_t kind;
    uint32_t endpoint;
    uint32_t timeout_ms;
    size_t out_size;
    size_t in_capacity;
    dtv_l3_transfer_complete_fn complete;
    void *callback_context;
};

struct fixture {
    dtv_l3_claim_result claim_result;
    dtv_l3_cancel_scope cancel_scope;
    dtv_l3_result submit_result;
    unsigned int claim_calls;
    unsigned int release_claim_calls;
    unsigned int submit_calls;
    unsigned int cancel_calls;
    uint64_t last_cancel_id;
    dtv_l3_provider_binding *last_cancel_binding;
    struct pending pending[MAX_PENDING];
    int pipe_timeout_conflict;
    int sync_complete;
    dtv_l3_result sync_result;
    uint8_t sync_data[MAX_PAYLOAD];
    size_t sync_size;
    uint8_t out_snapshot[MAX_PAYLOAD];
    size_t out_snapshot_size;
};

static void sleep_ms(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
}

static uint64_t monotonic_now_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static struct fixture fixture_new(void) {
    struct fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.claim_result =
        (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    fixture.cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
    fixture.submit_result = DTV_L3_ACCEPTED;
    return fixture;
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

static dtv_l3_claim_result provider_claim(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->claim_calls;
    return fixture->claim_result;
}

static dtv_l3_release_result provider_release_claim(
    void *context,
    dtv_l3_provider_binding *binding,
    uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->release_claim_calls;
    return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                                   DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
}

static dtv_l3_result provider_submit(
    void *context,
    dtv_l3_provider_binding *binding,
    uint64_t transfer_id,
    const dtv_l3_transfer_request *request,
    dtv_l3_transfer_complete_fn complete,
    void *callback_context) {
    struct fixture *fixture = context;
    struct pending *slot;
    unsigned int index;
    const uint32_t pipe =
        request->kind == DTV_L3_TRANSFER_CONTROL ? 0u : request->endpoint;

    ++fixture->submit_calls;
    for (index = 0; index < MAX_PENDING; ++index) {
        const struct pending *other = &fixture->pending[index];
        if (!other->used || other->endpoint != pipe) {
            continue;
        }
        if (other->timeout_ms != request->timeout_ms) {
            fixture->pipe_timeout_conflict = 1;
        }
    }
    /* D-F10: the Core must reject the conflict before this assertion. */
    assert(fixture->pipe_timeout_conflict == 0);
    if (request->out_size != 0 && request->out_data != NULL) {
        if (request->out_size > MAX_PAYLOAD) {
            return DTV_L3_INVALID;
        }
        memcpy(fixture->out_snapshot, request->out_data, request->out_size);
        fixture->out_snapshot_size = request->out_size;
    }
    if (fixture->sync_complete) {
        const dtv_l3_transfer_completion completion = {
            .result = fixture->sync_result,
            .data = fixture->sync_size == 0 ? NULL : fixture->sync_data,
            .size = fixture->sync_size,
            .actual_length = fixture->sync_size,
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
    slot->kind = request->kind;
    slot->endpoint = pipe;
    slot->timeout_ms = request->timeout_ms;
    slot->out_size = request->out_size;
    slot->in_capacity = request->in_capacity;
    slot->complete = complete;
    slot->callback_context = callback_context;
    return fixture->submit_result;
}

static dtv_l3_result provider_cancel(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint64_t transfer_id) {
    struct fixture *fixture = context;
    ++fixture->cancel_calls;
    fixture->last_cancel_id = transfer_id;
    fixture->last_cancel_binding = binding;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_provider provider_for(struct fixture *fixture) {
    return (dtv_l3_provider){.provider_context = fixture,
                             .claim = provider_claim,
                             .release_claim = provider_release_claim,
                             .submit = provider_submit,
                             .cancel = provider_cancel,
                             .cancel_scope = fixture->cancel_scope};
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

static dtv_l3_context *open_ready(dtv_l3_provider *provider) {
    dtv_l3_open_result opened = dtv_l3_open(provider, 1);
    dtv_l3_op_result result;
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return NULL;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return NULL;
    }
    return opened.owner;
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

static int close_and_release(dtv_l3_context *owner) {
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK) {
        return 0;
    }
    if (!wait_releasable(owner)) {
        return 0;
    }
    return dtv_l3_context_release(owner) == DTV_L3_OK;
}

static int start_transfer(dtv_l3_context *owner,
                          const dtv_l3_transfer_request *request,
                          dtv_l3_op **op) {
    if (dtv_l3_transfer_start(owner, request, op) != DTV_L3_ACCEPTED ||
        *op == NULL) {
        return 0;
    }
    return 1;
}

static dtv_l3_transfer_request bulk_request(uint32_t endpoint,
                                            uint32_t timeout_ms,
                                            const uint8_t *out_data,
                                            size_t out_size,
                                            size_t in_capacity) {
    dtv_l3_transfer_request request;
    memset(&request, 0, sizeof(request));
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = endpoint;
    request.timeout_ms = timeout_ms;
    request.out_data = out_data;
    request.out_size = out_size;
    request.in_capacity = in_capacity;
    request.claim_index = 0;
    return request;
}

static int fire_pending(struct fixture *fixture,
                        uint64_t id,
                        dtv_l3_result result,
                        const uint8_t *data,
                        size_t size,
                        size_t actual_length,
                        int actual_length_known) {
    struct pending slot;
    struct pending *found = find_pending(fixture, id);
    if (found == NULL) {
        return 0;
    }
    slot = *found;
    memset(found, 0, sizeof(*found));
    const dtv_l3_transfer_completion completion = {
        .result = result,
        .data = data,
        .size = size,
        .actual_length = actual_length,
        .actual_length_known = actual_length_known ? 1 : 0};
    slot.complete(slot.binding, slot.id, &completion,
                  slot.callback_context);
    return 1;
}

static uint64_t only_pending_id(struct fixture *fixture, uint64_t skip) {
    unsigned int index;
    for (index = 0; index < MAX_PENDING; ++index) {
        if (fixture->pending[index].used &&
            fixture->pending[index].id != skip) {
            return fixture->pending[index].id;
        }
    }
    return 0;
}

static int test_pipe_abort_out(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_op *op_a = NULL;
    dtv_l3_op *op_b = NULL;
    dtv_l3_op_result result;
    uint64_t id_a;
    uint64_t id_b;
    uint8_t payload_a[1] = {1};
    uint8_t payload_b[1] = {2};
    dtv_l3_transfer_request request;

    fixture.cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE;
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    request = bulk_request(0x01, 1000, payload_a, sizeof(payload_a), 0);
    if (!start_transfer(owner, &request, &op_a)) {
        return 2;
    }
    id_a = only_pending_id(&fixture, 0);
    request = bulk_request(0x01, 1000, payload_b, sizeof(payload_b), 0);
    if (!start_transfer(owner, &request, &op_b)) {
        return 3;
    }
    id_b = only_pending_id(&fixture, id_a);
    if (id_a == 0 || id_b == 0) {
        return 4;
    }
    if (dtv_l3_op_cancel(op_a) != DTV_L3_ACCEPTED ||
        fixture.cancel_calls != 1 || fixture.last_cancel_id != id_a) {
        return 5;
    }
    if (!fire_pending(&fixture, id_a, DTV_L3_CANCELLED, NULL, 0, 1, 1) ||
        !fire_pending(&fixture, id_b, DTV_L3_CANCELLED, NULL, 0, 1, 1)) {
        return 6;
    }
    if (!wait_op_terminal(op_a, &result) ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_OUTCOME_UNKNOWN ||
        result.cancel_cause != DTV_L3_CANCEL_CAUSE_REQUESTED) {
        return 7;
    }
    if (!wait_op_terminal(op_b, &result) ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_OUTCOME_UNKNOWN ||
        result.cancel_cause != DTV_L3_CANCEL_CAUSE_PROVIDER_ABORT) {
        return 8;
    }
    if (fixture.cancel_calls != 1) {
        return 9;
    }
    if (dtv_l3_op_release(op_a) != DTV_L3_OK ||
        dtv_l3_op_release(op_b) != DTV_L3_OK) {
        return 10;
    }
    return close_and_release(owner) ? 0 : 11;
}

static int test_pipe_abort_in_resubmit(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_op *op_a = NULL;
    dtv_l3_op *op_b = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    uint64_t id_a;
    uint64_t id_b;
    uint64_t old_b;
    uint64_t new_b;
    uint8_t response[2] = {0x11, 0x22};

    fixture.cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE;
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    request = bulk_request(0x81, 0, NULL, 0, 64);
    request.resubmit_on_provider_abort = 1;
    if (!start_transfer(owner, &request, &op_a)) {
        return 2;
    }
    id_a = only_pending_id(&fixture, 0);
    if (!start_transfer(owner, &request, &op_b)) {
        return 3;
    }
    id_b = only_pending_id(&fixture, id_a);
    if (id_a == 0 || id_b == 0) {
        return 4;
    }
    if (dtv_l3_op_cancel(op_a) != DTV_L3_ACCEPTED ||
        fixture.cancel_calls != 1 || fixture.last_cancel_id != id_a) {
        return 5;
    }
    if (!fire_pending(&fixture, id_a, DTV_L3_CANCELLED, NULL, 0, 0, 1)) {
        return 6;
    }
    old_b = id_b;
    if (!fire_pending(&fixture, old_b, DTV_L3_CANCELLED, NULL, 0, 0, 1)) {
        return 7;
    }
    new_b = only_pending_id(&fixture, id_a);
    if (new_b == 0 || new_b == old_b || fixture.submit_calls != 3) {
        return 8;
    }
    if (!fire_pending(&fixture, new_b, DTV_L3_OK, response,
                      sizeof(response), sizeof(response), 1)) {
        return 9;
    }
    if (!wait_op_terminal(op_b, &result) ||
        result.state != DTV_L3_OP_DONE || result.result != DTV_L3_OK ||
        result.cancel_cause != DTV_L3_CANCEL_CAUSE_NONE ||
        result.provider_abort_resubmits != 1 || result.data_size != 2 ||
        result.data == NULL || result.data[0] != 0x11) {
        return 10;
    }
    if (fixture.cancel_calls != 1) {
        return 11;
    }
    if (dtv_l3_op_release(op_a) != DTV_L3_OK ||
        dtv_l3_op_release(op_b) != DTV_L3_OK) {
        return 12;
    }
    return close_and_release(owner) ? 0 : 13;
}

static int test_shared_in_deadline(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_op *response_op = NULL;
    dtv_l3_op *ts_op = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    uint64_t response_id;
    uint64_t ts_id;
    uint8_t late[2] = {0xaa, 0xbb};
    uint8_t ts_data[4] = {0x01, 0x02, 0x03, 0x04};

    fixture.cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE;
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    request = bulk_request(0x81, 0, NULL, 0, 64);
    request.deadline_ns = monotonic_now_ns() + 50000000ull;
    if (!start_transfer(owner, &request, &response_op)) {
        return 2;
    }
    response_id = only_pending_id(&fixture, 0);
    request = bulk_request(0x81, 0, NULL, 0, 64);
    if (!start_transfer(owner, &request, &ts_op)) {
        return 3;
    }
    ts_id = only_pending_id(&fixture, response_id);
    if (response_id == 0 || ts_id == 0) {
        return 4;
    }
    /* No host poll, wait, or close loop: T2 owns the deadline timer. */
    sleep_ms(150);
    if (!wait_op_terminal(response_op, &result) ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_TIMEOUT ||
        result.cancel_cause != DTV_L3_CANCEL_CAUSE_NONE) {
        return 5;
    }
    if (fixture.cancel_calls != 0) {
        return 6;
    }
    if (!fire_pending(&fixture, response_id, DTV_L3_OK, late, sizeof(late),
                      sizeof(late), 1)) {
        return 7;
    }
    if (dtv_l3_op_poll(response_op, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_TIMEOUT) {
        return 8;
    }
    if (!fire_pending(&fixture, ts_id, DTV_L3_OK, ts_data, sizeof(ts_data),
                      sizeof(ts_data), 1)) {
        return 9;
    }
    if (!wait_op_terminal(ts_op, &result) ||
        result.state != DTV_L3_OP_DONE || result.result != DTV_L3_OK ||
        result.data_size != sizeof(ts_data) || result.data == NULL ||
        result.data[0] != ts_data[0]) {
        return 10;
    }
    if (fixture.cancel_calls != 0) {
        return 11;
    }
    if (dtv_l3_op_release(response_op) != DTV_L3_OK ||
        dtv_l3_op_release(ts_op) != DTV_L3_OK) {
        return 12;
    }
    return close_and_release(owner) ? 0 : 13;
}

static int test_disconnect_late_callback(void) {
    struct fixture fixture = fixture_new();
    struct fixture other_fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_provider other_provider;
    dtv_l3_context *owner;
    dtv_l3_context *other_owner;
    dtv_l3_op *op_a = NULL;
    dtv_l3_op *op_b = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    dtv_l3_close_status close_status;
    dtv_l3_open_status open_status;
    dtv_l3_op *blocked = NULL;
    uint64_t id_a;
    uint64_t id_b;

    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    request = bulk_request(0x81, 0, NULL, 0, 64);
    if (!start_transfer(owner, &request, &op_a)) {
        return 2;
    }
    id_a = only_pending_id(&fixture, 0);
    if (!start_transfer(owner, &request, &op_b)) {
        return 3;
    }
    id_b = only_pending_id(&fixture, id_a);
    if (id_a == 0 || id_b == 0) {
        return 4;
    }
    if (!fire_pending(&fixture, id_a, DTV_L3_DISCONNECTED, NULL, 0, 0, 1)) {
        return 5;
    }
    if (!wait_op_terminal(op_a, &result) ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_DISCONNECTED) {
        return 6;
    }
    if (dtv_l3_op_poll(op_b, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_DISCONNECTED) {
        return 7;
    }
    if (dtv_l3_close_status_get(owner, &close_status) != DTV_L3_OK ||
        !close_status.disconnected ||
        dtv_l3_open_status_get(owner, &open_status) != DTV_L3_OK ||
        !open_status.disconnected) {
        return 8;
    }
    if (dtv_l3_transfer_start(owner, &request, &blocked) !=
            DTV_L3_DISCONNECTED ||
        blocked != NULL || fixture.submit_calls != 2) {
        return 9;
    }
    if (dtv_l3_op_release(op_a) != DTV_L3_OK ||
        dtv_l3_op_release(op_b) != DTV_L3_OK) {
        return 10;
    }
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK) {
        return 11;
    }
    if (dtv_l3_close_status_get(owner, &close_status) != DTV_L3_OK ||
        close_status.all_ops_recovered) {
        return 12;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_BUSY) {
        return 13;
    }
    if (!fire_pending(&fixture, id_b, DTV_L3_DISCONNECTED, NULL, 0, 0, 1)) {
        return 14;
    }
    if (!wait_releasable(owner)) {
        return 15;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 16;
    }

    other_provider = provider_for(&other_fixture);
    other_owner = open_ready(&other_provider);
    if (other_owner == NULL) {
        return 17;
    }
    return close_and_release(other_owner) ? 0 : 18;
}

static int test_submit_sync_and_copies(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    uint8_t payload[4] = {1, 2, 3, 4};

    fixture.sync_complete = 1;
    fixture.sync_result = DTV_L3_OK;
    fixture.sync_data[0] = 0xaa;
    fixture.sync_data[1] = 0xbb;
    fixture.sync_size = 2;
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    request = bulk_request(0x01, 1000, payload, sizeof(payload), 8);
    if (!start_transfer(owner, &request, &op)) {
        return 2;
    }
    payload[0] = 0xee;
    if (fixture.out_snapshot_size != sizeof(payload) ||
        fixture.out_snapshot[0] != 1 || fixture.out_snapshot[3] != 4) {
        return 3;
    }
    if (!wait_op_terminal(op, &result) ||
        result.state != DTV_L3_OP_DONE || result.result != DTV_L3_OK ||
        result.data_size != 2 || result.data == NULL ||
        result.data[0] != 0xaa) {
        return 4;
    }
    fixture.sync_data[0] = 0x00;
    if (dtv_l3_op_poll(op, &result) != DTV_L3_OK || result.data == NULL ||
        result.data[0] != 0xaa) {
        return 5;
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK) {
        return 6;
    }
    fixture.sync_complete = 0;
    fixture.submit_result = DTV_L3_FAILED;
    request = bulk_request(0x01, 1000, payload, sizeof(payload), 8);
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_FAILED ||
        op != NULL || fixture.submit_calls != 2) {
        return 7;
    }
    return close_and_release(owner) ? 0 : 8;
}

static int test_control_direction(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    uint8_t payload[1] = {0x33};

    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    memset(&request, 0, sizeof(request));
    request.kind = DTV_L3_TRANSFER_CONTROL;
    request.endpoint = 0;
    request.timeout_ms = 1000;
    request.claim_index = 0;
    request.control.request_type = 0x80;
    request.out_data = payload;
    request.out_size = sizeof(payload);
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_INVALID ||
        op != NULL || fixture.submit_calls != 0) {
        return 2;
    }
    request.control.request_type = 0x40;
    request.out_data = NULL;
    request.out_size = 0;
    request.in_capacity = 8;
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_INVALID ||
        op != NULL || fixture.submit_calls != 0) {
        return 3;
    }
    request.control.request_type = 0xc0;
    request.in_capacity = 8;
    if (!start_transfer(owner, &request, &op) ||
        fixture.submit_calls != 1) {
        return 4;
    }
    if (!fire_pending(&fixture, only_pending_id(&fixture, 0), DTV_L3_OK, NULL,
                      0, 0, 1)) {
        return 5;
    }
    if (!wait_op_terminal(op, &result) ||
        result.state != DTV_L3_OP_DONE || result.result != DTV_L3_OK) {
        return 6;
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK) {
        return 7;
    }
    return close_and_release(owner) ? 0 : 8;
}

static int test_pipe_timeout_conflict(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_op *op_a = NULL;
    dtv_l3_op *op_b = NULL;
    dtv_l3_op *op_c = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    uint8_t payload[1] = {0x01};

    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    request = bulk_request(0x01, 1000, payload, sizeof(payload), 0);
    if (!start_transfer(owner, &request, &op_a)) {
        return 2;
    }
    request = bulk_request(0x01, 2000, payload, sizeof(payload), 0);
    if (dtv_l3_transfer_start(owner, &request, &op_b) != DTV_L3_INVALID ||
        op_b != NULL || fixture.submit_calls != 1 ||
        fixture.pipe_timeout_conflict) {
        return 3;
    }
    request = bulk_request(0x01, 0, payload, sizeof(payload), 0);
    if (dtv_l3_transfer_start(owner, &request, &op_b) != DTV_L3_INVALID ||
        op_b != NULL || fixture.submit_calls != 1) {
        return 4;
    }
    request = bulk_request(0x02, 0, payload, sizeof(payload), 0);
    if (!start_transfer(owner, &request, &op_c) ||
        fixture.submit_calls != 2) {
        return 5;
    }
    if (!fire_pending(&fixture, only_pending_id(&fixture, 0), DTV_L3_OK, NULL,
                      0, 0, 1)) {
        return 6;
    }
    if (!wait_op_terminal(op_a, &result) ||
        result.state != DTV_L3_OP_DONE) {
        return 7;
    }
    if (!fire_pending(&fixture, only_pending_id(&fixture, 0), DTV_L3_OK, NULL,
                      0, 0, 1)) {
        return 8;
    }
    if (!wait_op_terminal(op_c, &result) ||
        result.state != DTV_L3_OP_DONE) {
        return 9;
    }
    if (fixture.pipe_timeout_conflict) {
        return 10;
    }
    if (dtv_l3_op_release(op_a) != DTV_L3_OK ||
        dtv_l3_op_release(op_c) != DTV_L3_OK) {
        return 11;
    }
    return close_and_release(owner) ? 0 : 12;
}

struct context_run {
    struct fixture fixture;
    int result;
    int successful;
    uint64_t cancellation_id;
};

static void *run_context(void *argument) {
    struct context_run *run = argument;
    dtv_l3_provider provider = provider_for(&run->fixture);
    dtv_l3_context *owner = open_ready(&provider);
    dtv_l3_transfer_request request;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result result;
    uint64_t id;

    run->successful = 0;
    run->result = 1;
    if (owner == NULL) {
        return NULL;
    }
    request = bulk_request(0x81, 0, NULL, 0, 64);
    if (!start_transfer(owner, &request, &op)) {
        run->result = 2;
        return NULL;
    }
    id = only_pending_id(&run->fixture, 0);
    if (id == 0 || dtv_l3_op_cancel(op) != DTV_L3_ACCEPTED) {
        run->result = 3;
        return NULL;
    }
    run->cancellation_id = run->fixture.last_cancel_id;
    if (!fire_pending(&run->fixture, id, DTV_L3_CANCELLED, NULL, 0, 0, 1) ||
        !wait_op_terminal(op, &result) ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_CANCELLED ||
        result.cancel_cause != DTV_L3_CANCEL_CAUSE_REQUESTED) {
        run->result = 4;
        return NULL;
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK ||
        !close_and_release(owner)) {
        run->result = 5;
        return NULL;
    }
    run->successful = 1;
    run->result = 0;
    return NULL;
}

static int test_two_contexts(void) {
    struct context_run a;
    struct context_run b;
    pthread_t thread_a;
    pthread_t thread_b;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.fixture = fixture_new();
    b.fixture = fixture_new();
    if (pthread_create(&thread_a, NULL, run_context, &a) != 0) {
        return 1;
    }
    if (pthread_create(&thread_b, NULL, run_context, &b) != 0) {
        pthread_join(thread_a, NULL);
        return 2;
    }
    pthread_join(thread_a, NULL);
    pthread_join(thread_b, NULL);
    if (!a.successful || !b.successful) {
        return 3;
    }
    if (a.fixture.cancel_calls != 1 || b.fixture.cancel_calls != 1) {
        return 4;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 64;
    }
    if (strcmp(argv[1], "pipe-abort-out") == 0) {
        return test_pipe_abort_out();
    }
    if (strcmp(argv[1], "pipe-abort-in") == 0) {
        return test_pipe_abort_in_resubmit();
    }
    if (strcmp(argv[1], "shared-in-deadline") == 0) {
        return test_shared_in_deadline();
    }
    if (strcmp(argv[1], "disconnect-late") == 0) {
        return test_disconnect_late_callback();
    }
    if (strcmp(argv[1], "submit-sync-copy") == 0) {
        return test_submit_sync_and_copies();
    }
    if (strcmp(argv[1], "control-direction") == 0) {
        return test_control_direction();
    }
    if (strcmp(argv[1], "pipe-timeout") == 0) {
        return test_pipe_timeout_conflict();
    }
    if (strcmp(argv[1], "two-contexts") == 0) {
        return test_two_contexts();
    }
    return 65;
}
