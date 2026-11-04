/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <dtv/l3_owner.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_PENDING 16
#define MAX_LOG_MESSAGES 32

/* Mirrors the prototype test seam bits in owner_engine.h. */
#define INJECT_ENTRY 0x0001u
#define INJECT_OPEN_CONTEXT 0x0002u
#define INJECT_OPEN_OP 0x0004u
#define INJECT_START_PAYLOAD 0x0008u
#define INJECT_START_SUBMIT 0x0010u
#define INJECT_CLAIM 0x0020u
#define INJECT_CANCEL 0x0040u
#define INJECT_RELEASE_CLAIM 0x0080u
#define INJECT_COMPLETION_COPY 0x0100u
#define INJECT_TS_DELIVER 0x0200u

extern void l3_test_set_lane_start_failure_mask(int mask);
extern void l3_test_set_exception_mask(unsigned int mask);

struct pending {
    int used;
    dtv_l3_provider_binding *binding;
    uint64_t id;
    dtv_l3_transfer_complete_fn complete;
    void *callback_context;
};

struct log_record {
    unsigned int calls;
    unsigned int errors;
    unsigned int warnings;
    unsigned int infos;
    unsigned int debugs;
    dtv_l3_log_level last_level;
    int check_userdata;
    void *expected_userdata;
    int reentry;
    dtv_l3_context *owner;
    dtv_l3_op *op;
    dtv_l3_result inner_close;
    dtv_l3_result inner_wait;
    dtv_l3_result inner_release;
};

struct fixture {
    dtv_l3_claim_result claim_result;
    dtv_l3_result submit_result;
    int sync_complete;
    dtv_l3_result sync_result;
    uint8_t sync_data[64];
    size_t sync_size;
    unsigned int claim_calls;
    unsigned int release_claim_calls;
    unsigned int submit_calls;
    unsigned int cancel_calls;
    unsigned int scope_calls;
    unsigned int tune_calls;
    unsigned int bridge_start_calls;
    unsigned int bridge_stop_calls;
    unsigned int attach_calls;
    unsigned int stream_stop_calls;
    int release_claim_fail_once;
    struct pending pending[MAX_PENDING];
    dtv_l3_ts_deliver_fn deliver;
    void *deliver_context;
    dtv_l3_log_level log_min;
    struct log_record logs;
};

static void sleep_ms(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
}

static void log_callback(void *userdata,
                         dtv_l3_log_level level,
                         const char *fn,
                         const char *message) {
    struct log_record *record = userdata;
    (void)fn;
    (void)message;
    ++record->calls;
    record->last_level = level;
    if (level == DTV_L3_LOG_ERROR) {
        ++record->errors;
    } else if (level == DTV_L3_LOG_WARN) {
        ++record->warnings;
    } else if (level == DTV_L3_LOG_INFO) {
        ++record->infos;
    } else {
        ++record->debugs;
    }
    if (record->reentry) {
        record->reentry = 0;
        record->inner_close = dtv_l3_close(record->owner, 0);
        record->inner_wait = record->op == NULL
                                 ? DTV_L3_OK
                                 : dtv_l3_op_wait(record->op, 0);
        record->inner_release = dtv_l3_context_release(record->owner);
    }
}

static struct fixture fixture_new(void) {
    struct fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.claim_result =
        (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    fixture.submit_result = DTV_L3_ACCEPTED;
    fixture.log_min = DTV_L3_LOG_ERROR;
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
    if (fixture->release_claim_fail_once) {
        fixture->release_claim_fail_once = 0;
        return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCENCE_UNKNOWN,
                                       DTV_L3_CLAIM_RETAINED,
                                       DTV_L3_RESTORE_UNKNOWN, DTV_L3_FAILED};
    }
    return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED,
                                   DTV_L3_CLAIM_NONE,
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
    (void)request;
    ++fixture->submit_calls;
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
    slot->complete = complete;
    slot->callback_context = callback_context;
    return fixture->submit_result;
}

static dtv_l3_result provider_cancel(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint64_t transfer_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)transfer_id;
    ++fixture->cancel_calls;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result provider_scope_query(void *context,
                                          uint32_t receiver_id,
                                          dtv_l3_scope_info *scope) {
    struct fixture *fixture = context;
    (void)receiver_id;
    ++fixture->scope_calls;
    scope->bridge_id = 1;
    scope->compatibility = DTV_L3_LEASE_SHAREABLE;
    return DTV_L3_OK;
}

static dtv_l3_result provider_tune(void *context,
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

static dtv_l3_result provider_bridge_start(void *context,
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

static dtv_l3_result provider_bridge_stop(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t bridge_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)bridge_id;
    ++fixture->bridge_stop_calls;
    return DTV_L3_OK;
}

static dtv_l3_result provider_stream_attach(void *context,
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

static dtv_l3_result provider_stream_stop(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint64_t stream_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)stream_id;
    ++fixture->stream_stop_calls;
    return DTV_L3_OK;
}

static dtv_l3_provider provider_for(struct fixture *fixture) {
    return (dtv_l3_provider){
        .provider_context = fixture,
        .claim = provider_claim,
        .release_claim = provider_release_claim,
        .submit = provider_submit,
        .cancel = provider_cancel,
        .scope_query = provider_scope_query,
        .bridge_start = provider_bridge_start,
        .bridge_stop = provider_bridge_stop,
        .stream_attach = provider_stream_attach,
        .stream_stop = provider_stream_stop,
        .tune = provider_tune,
        .cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER,
        .logger = {log_callback, &fixture->logs, fixture->log_min}};
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

static uint64_t only_pending_id(struct fixture *fixture) {
    unsigned int index;
    for (index = 0; index < MAX_PENDING; ++index) {
        if (fixture->pending[index].used) {
            return fixture->pending[index].id;
        }
    }
    return 0;
}

static int fire_pending(struct fixture *fixture,
                        uint64_t id,
                        dtv_l3_result result,
                        const uint8_t *data,
                        size_t size) {
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
        .actual_length = size,
        .actual_length_known = 1};
    slot.complete(slot.binding, slot.id, &completion, slot.callback_context);
    return 1;
}

static int start_one(dtv_l3_context *owner,
                     struct fixture *fixture,
                     uint32_t endpoint,
                     uint32_t timeout_ms,
                     dtv_l3_op **op) {
    const dtv_l3_transfer_request request =
        bulk_request(endpoint, timeout_ms, NULL, 0, 64);
    (void)fixture;
    return dtv_l3_transfer_start(owner, &request, op) == DTV_L3_ACCEPTED &&
           *op != NULL;
}

static int test_entry_guards(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_open_result opened;
    dtv_l3_open_status open_status;
    dtv_l3_close_status close_status;
    dtv_l3_op_result op_result;
    dtv_l3_lease_status lease_status;
    dtv_l3_stream_status stream_status;
    dtv_l3_op *op = NULL;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_transfer_request request;
    uint64_t id;
    uint64_t programmed = 0;
    uint64_t revision = 0;

    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }

    l3_test_set_exception_mask(INJECT_ENTRY);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL) {
        return 2;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_open_status_get(owner, &open_status) != DTV_L3_FAILED) {
        return 3;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_close(owner, 0) != DTV_L3_FAILED) {
        return 4;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_close_status_get(owner, &close_status) != DTV_L3_FAILED) {
        return 5;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_cleanup_retry(owner) != DTV_L3_FAILED) {
        return 6;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_reader_acquire(owner, 0, NULL) != DTV_L3_FAILED) {
        return 7;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_lnb_set(owner, 0, 0) != DTV_L3_FAILED) {
        return 8;
    }

    request = bulk_request(0x81, 0, NULL, 0, 64);
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_FAILED ||
        op != NULL) {
        return 9;
    }
    if (!start_one(owner, &fixture, 0x81, 0, &op)) {
        return 10;
    }
    id = only_pending_id(&fixture);
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_op_cancel(op) != DTV_L3_FAILED) {
        return 11;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_op_poll(op, &op_result) != DTV_L3_FAILED) {
        return 12;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_op_wait(op, 0) != DTV_L3_FAILED) {
        return 13;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_op_release(op) != DTV_L3_FAILED) {
        return 14;
    }
    if (!fire_pending(&fixture, id, DTV_L3_OK, NULL, 0) ||
        !wait_op_terminal(op, &op_result) ||
        op_result.state != DTV_L3_OP_DONE ||
        dtv_l3_op_release(op) != DTV_L3_OK) {
        return 15;
    }

    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_lease_acquire(owner, 1, &lease) != DTV_L3_FAILED ||
        lease != NULL) {
        return 16;
    }
    if (dtv_l3_lease_acquire(owner, 1, &lease) != DTV_L3_OK ||
        lease == NULL) {
        return 17;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_lease_tune(lease, 1000, &programmed, &revision) !=
        DTV_L3_FAILED) {
        return 18;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_lease_start(lease) != DTV_L3_FAILED) {
        return 19;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_lease_attach(lease, &stream) != DTV_L3_FAILED ||
        stream != NULL) {
        return 20;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_lease_status_get(lease, &lease_status) != DTV_L3_FAILED) {
        return 21;
    }
    if (dtv_l3_lease_tune(lease, 1000, &programmed, &revision) != DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK ||
        stream == NULL) {
        return 22;
    }

    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_FAILED) {
        return 23;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_stream_read_ts(stream, NULL, 0, NULL, NULL) !=
        DTV_L3_FAILED) {
        return 24;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_stream_read_ts_wait(stream, NULL, 0, NULL, NULL, 0) !=
        DTV_L3_FAILED) {
        return 25;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_FAILED) {
        return 26;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
        DTV_L3_FAILED) {
        return 27;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_stream_destroy(stream) != DTV_L3_FAILED) {
        return 28;
    }
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
            DTV_L3_OK ||
        dtv_l3_stream_destroy(stream) != DTV_L3_OK) {
        return 29;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_lease_release(lease) != DTV_L3_FAILED) {
        return 30;
    }
    if (dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 31;
    }

    if (dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner)) {
        return 32;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_context_release(owner) != DTV_L3_FAILED) {
        return 33;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 34;
    }
    return 0;
}

static int test_open_allocation_guards(void) {
    struct fixture fixture;
    dtv_l3_provider provider;
    dtv_l3_open_result opened;

    fixture = fixture_new();
    provider = provider_for(&fixture);
    l3_test_set_exception_mask(INJECT_OPEN_CONTEXT);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL || fixture.claim_calls != 0) {
        return 1;
    }
    l3_test_set_exception_mask(INJECT_OPEN_OP);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL || fixture.claim_calls != 0) {
        return 2;
    }
    l3_test_set_lane_start_failure_mask(2);
    opened = dtv_l3_open(&provider, 1);
    l3_test_set_lane_start_failure_mask(0);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL || fixture.claim_calls != 0) {
        return 3;
    }
    return 0;
}

static int test_transfer_allocation_guards(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    uint8_t payload[2] = {0x11, 0x22};
    uint64_t id;

    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    request = bulk_request(0x01, 1000, payload, sizeof(payload), 0);
    l3_test_set_exception_mask(INJECT_START_PAYLOAD);
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_FAILED ||
        op != NULL) {
        return 2;
    }
    l3_test_set_exception_mask(INJECT_START_SUBMIT);
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_FAILED ||
        op != NULL || fixture.submit_calls != 0) {
        return 3;
    }
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == NULL) {
        return 4;
    }
    id = only_pending_id(&fixture);
    if (!fire_pending(&fixture, id, DTV_L3_OK, NULL, 0) ||
        !wait_op_terminal(op, &result) || result.state != DTV_L3_OP_DONE ||
        dtv_l3_op_release(op) != DTV_L3_OK) {
        return 5;
    }
    return close_and_release(owner) ? 0 : 6;
}

static int test_claim_exception(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    dtv_l3_open_status status;

    l3_test_set_exception_mask(INJECT_CLAIM);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED || result.result != DTV_L3_FAILED ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 2;
    }
    if (dtv_l3_open_status_get(opened.owner, &status) != DTV_L3_OK ||
        status.state != DTV_L3_OPEN_FAILED || fixture.claim_calls != 0) {
        return 3;
    }
    if (!close_and_release(opened.owner)) {
        return 4;
    }
    return 0;
}

static int test_cancel_exception(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result result;
    uint64_t id;

    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (!start_one(owner, &fixture, 0x81, 0, &op)) {
        return 2;
    }
    id = only_pending_id(&fixture);
    l3_test_set_exception_mask(INJECT_CANCEL);
    if (dtv_l3_op_cancel(op) != DTV_L3_FAILED || fixture.cancel_calls != 0) {
        return 3;
    }
    if (!fire_pending(&fixture, id, DTV_L3_OK, NULL, 0) ||
        !wait_op_terminal(op, &result) || result.state != DTV_L3_OP_DONE ||
        dtv_l3_op_release(op) != DTV_L3_OK) {
        return 4;
    }
    return close_and_release(owner) ? 0 : 5;
}

static int test_completion_copy_exception(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    uint8_t data[4] = {1, 2, 3, 4};

    fixture.sync_complete = 1;
    fixture.sync_result = DTV_L3_OK;
    memcpy(fixture.sync_data, data, sizeof(data));
    fixture.sync_size = sizeof(data);
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    request = bulk_request(0x81, 0, NULL, 0, 64);
    l3_test_set_exception_mask(INJECT_COMPLETION_COPY);
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == NULL) {
        return 2;
    }
    if (!wait_op_terminal(op, &result) ||
        result.state != DTV_L3_OP_FAILED || result.result != DTV_L3_FAILED ||
        result.data_size != 0) {
        return 3;
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK) {
        return 4;
    }
    return close_and_release(owner) ? 0 : 5;
}

static int test_release_claim_exception(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_close_status status;

    fixture.release_claim_fail_once = 1;
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (dtv_l3_close(owner, 2000) != DTV_L3_TIMEOUT) {
        return 2;
    }
    if (dtv_l3_close_status_get(owner, &status) != DTV_L3_OK ||
        status.releasable || status.cleanup_complete ||
        fixture.release_claim_calls != 1) {
        return 3;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_BUSY) {
        return 4;
    }
    if (dtv_l3_cleanup_retry(owner) != DTV_L3_ACCEPTED) {
        return 5;
    }
    if (!wait_releasable(owner)) {
        return 6;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 7;
    }
    return 0;
}

static int test_ts_deliver_exception(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_read_state state;
    size_t bytes = 0;
    uint8_t ts_packet[188];
    uint8_t out[188];
    uint64_t programmed = 0;
    uint64_t revision = 0;

    memset(ts_packet, 0x47, sizeof(ts_packet));
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (dtv_l3_lease_acquire(owner, 1, &lease) != DTV_L3_OK ||
        dtv_l3_lease_tune(lease, 1000, &programmed, &revision) != DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK ||
        stream == NULL || fixture.deliver == NULL) {
        return 2;
    }
    l3_test_set_exception_mask(INJECT_TS_DELIVER);
    fixture.deliver(fixture.deliver_context, ts_packet, sizeof(ts_packet));
    if (dtv_l3_stream_read_ts(stream, out, sizeof(out), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_WAIT || bytes != 0) {
        return 3;
    }
    fixture.deliver(fixture.deliver_context, ts_packet, sizeof(ts_packet));
    if (dtv_l3_stream_read_ts(stream, out, sizeof(out), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_DATA || bytes != sizeof(out) ||
        out[0] != 0x47) {
        return 4;
    }
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
            DTV_L3_OK ||
        dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 5;
    }
    return close_and_release(owner) ? 0 : 6;
}

static int test_logging_levels_and_context(void) {
    struct fixture fixture_a = fixture_new();
    struct fixture fixture_b = fixture_new();
    dtv_l3_provider provider_a;
    dtv_l3_provider provider_b;
    dtv_l3_context *owner_a;
    dtv_l3_context *owner_b;
    dtv_l3_op_result result;

    fixture_a.log_min = DTV_L3_LOG_INFO;
    fixture_b.log_min = DTV_L3_LOG_INFO;
    provider_a = provider_for(&fixture_a);
    provider_b = provider_for(&fixture_b);
    owner_a = open_ready(&provider_a);
    owner_b = open_ready(&provider_b);
    if (owner_a == NULL || owner_b == NULL) {
        return 1;
    }
    if (fixture_a.logs.infos == 0 || fixture_b.logs.infos == 0) {
        return 2;
    }
    if (fixture_a.logs.errors != 0 || fixture_b.logs.errors != 0) {
        return 3;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_close(owner_a, 0) != DTV_L3_FAILED) {
        return 4;
    }
    if (fixture_a.logs.errors == 0 || fixture_a.logs.last_level !=
                                          DTV_L3_LOG_ERROR) {
        return 5;
    }
    if (fixture_b.logs.errors != 0) {
        return 6;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_close(owner_b, 0) != DTV_L3_FAILED ||
        fixture_b.logs.errors == 0) {
        return 7;
    }

    if (dtv_l3_close(owner_a, 2000) != DTV_L3_OK ||
        !wait_releasable(owner_a) ||
        dtv_l3_context_release(owner_a) != DTV_L3_OK) {
        return 8;
    }
    if (dtv_l3_close(owner_b, 2000) != DTV_L3_OK ||
        !wait_releasable(owner_b) ||
        dtv_l3_context_release(owner_b) != DTV_L3_OK) {
        return 9;
    }

    {
        struct fixture fixture_c = fixture_new();
        dtv_l3_provider provider_c;
        dtv_l3_context *owner_c;
        fixture_c.log_min = DTV_L3_LOG_WARN;
        provider_c = provider_for(&fixture_c);
        l3_test_set_exception_mask(INJECT_CLAIM);
        {
            dtv_l3_open_result opened = dtv_l3_open(&provider_c, 1);
            if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
                opened.op == NULL) {
                return 10;
            }
            owner_c = opened.owner;
            if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
                dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
                result.state != DTV_L3_OP_FAILED ||
                dtv_l3_op_release(opened.op) != DTV_L3_OK) {
                return 11;
            }
        }
        if (fixture_c.logs.infos != 0 || fixture_c.logs.warnings == 0) {
            return 12;
        }
        if (!close_and_release(owner_c)) {
            return 13;
        }
    }
    return 0;
}

static int test_log_reentry(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;

    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    fixture.logs.reentry = 1;
    fixture.logs.owner = owner;
    fixture.logs.op = NULL;
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_close(owner, 0) != DTV_L3_FAILED) {
        return 2;
    }
    if (fixture.logs.inner_close != DTV_L3_ACCEPTED &&
        fixture.logs.inner_close != DTV_L3_OK) {
        return 3;
    }
    if (fixture.logs.inner_release != DTV_L3_BUSY) {
        return 4;
    }
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner) ||
        dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 5;
    }
    return 0;
}

static int test_no_stderr(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    int saved_stderr;
    int pipe_fds[2];
    char buffer[256];
    ssize_t bytes;

    if (pipe(pipe_fds) != 0) {
        return 1;
    }
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 2;
    }
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        close(saved_stderr);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 3;
    }
    if (dup2(pipe_fds[1], STDERR_FILENO) < 0) {
        close(saved_stderr);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 4;
    }
    l3_test_set_exception_mask(INJECT_ENTRY);
    if (dtv_l3_close(owner, 0) != DTV_L3_FAILED) {
        return 5;
    }
    l3_test_set_exception_mask(INJECT_START_SUBMIT);
    {
        dtv_l3_transfer_request request =
            bulk_request(0x81, 0, NULL, 0, 64);
        dtv_l3_op *op = NULL;
        if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_FAILED ||
            op != NULL) {
            return 6;
        }
    }
    if (dup2(saved_stderr, STDERR_FILENO) < 0) {
        return 7;
    }
    close(saved_stderr);
    close(pipe_fds[1]);
    bytes = read(pipe_fds[0], buffer, sizeof(buffer));
    close(pipe_fds[0]);
    if (bytes != 0) {
        return 8;
    }
    if (fixture.logs.errors == 0) {
        return 9;
    }
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner) ||
        dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 10;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 64;
    }
    if (strcmp(argv[1], "entry") == 0) {
        return test_entry_guards();
    }
    if (strcmp(argv[1], "open-alloc") == 0) {
        return test_open_allocation_guards();
    }
    if (strcmp(argv[1], "transfer-alloc") == 0) {
        return test_transfer_allocation_guards();
    }
    if (strcmp(argv[1], "claim-exception") == 0) {
        return test_claim_exception();
    }
    if (strcmp(argv[1], "cancel-exception") == 0) {
        return test_cancel_exception();
    }
    if (strcmp(argv[1], "completion-copy") == 0) {
        return test_completion_copy_exception();
    }
    if (strcmp(argv[1], "release-claim") == 0) {
        return test_release_claim_exception();
    }
    if (strcmp(argv[1], "ts-deliver") == 0) {
        return test_ts_deliver_exception();
    }
    if (strcmp(argv[1], "logging") == 0) {
        return test_logging_levels_and_context();
    }
    if (strcmp(argv[1], "reentry") == 0) {
        return test_log_reentry();
    }
    if (strcmp(argv[1], "stderr") == 0) {
        return test_no_stderr();
    }
    return 65;
}
