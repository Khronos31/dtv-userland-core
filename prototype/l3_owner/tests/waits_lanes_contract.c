/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <dtv/l3_owner.h>

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_STREAMS 8

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define L3_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || \
    __has_feature(memory_sanitizer)
#define L3_SANITIZED 1
#else
#define L3_SANITIZED 0
#endif
#else
#define L3_SANITIZED 0
#endif

#define L3_CHECK_TASK_COUNT (!L3_SANITIZED)

struct fixture {
    atomic_int hold_claim;
    atomic_int claim_entered;
    atomic_int allow_claim;
    atomic_int hold_cancel;
    atomic_int claim_cancel_called;
    unsigned int claim_calls;
    unsigned int claim_cancel_calls;
    uint32_t claim_cancel_index;
    uint32_t claim_index;
    dtv_l3_provider_binding *claim_cancel_binding;
    dtv_l3_provider_binding *claim_binding;
    int claim_cancel_unblocks;
    int null_claim_cancel;
    unsigned int release_claim_calls;
    unsigned int cancel_calls;
    unsigned int start_calls;
    unsigned int stop_calls;
    unsigned int tune_calls;
    unsigned int attach_calls;
    unsigned int stream_stop_calls;
    dtv_l3_context *owner;
    dtv_l3_op *op;
    int reentry_mode;
    int complete_on_cancel;
    dtv_l3_result inner_close;
    dtv_l3_result inner_wait;
    dtv_l3_result inner_release;
    dtv_l3_transfer_complete_fn complete;
    void *complete_context;
    dtv_l3_provider_binding *complete_binding;
    uint64_t complete_id;
    dtv_l3_ts_deliver_fn deliver[MAX_STREAMS];
    void *deliver_context[MAX_STREAMS];
};

extern void l3_test_set_lane_start_failure_mask(int mask);

static void sleep_ms(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
}

static int count_tasks(void) {
    DIR *directory = opendir("/proc/self/task");
    struct dirent *entry;
    int count = 0;
    if (directory == NULL) {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

static int wait_task_count(int expected) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (count_tasks() == expected) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

static dtv_l3_claim_result provider_claim(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t claim_index) {
    struct fixture *fixture = context;
    ++fixture->claim_calls;
    fixture->claim_binding = binding;
    fixture->claim_index = claim_index;
    if (atomic_load(&fixture->hold_claim)) {
        atomic_store(&fixture->claim_entered, 1);
        while (!atomic_load(&fixture->allow_claim) &&
               !(fixture->claim_cancel_unblocks &&
                 atomic_load(&fixture->claim_cancel_called))) {
            sched_yield();
        }
    }
    if (fixture->reentry_mode == 1 && fixture->owner != NULL) {
        fixture->inner_close = dtv_l3_close(fixture->owner, 100);
        fixture->inner_wait = dtv_l3_op_wait(fixture->op, 100);
        fixture->inner_release = dtv_l3_context_release(fixture->owner);
    }
    return (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

static dtv_l3_result provider_claim_cancel(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t claim_index) {
    struct fixture *fixture = context;
    ++fixture->claim_cancel_calls;
    fixture->claim_cancel_index = claim_index;
    fixture->claim_cancel_binding = binding;
    atomic_store(&fixture->claim_cancel_called, 1);
    return DTV_L3_ACCEPTED;
}

static dtv_l3_release_result provider_release_claim(
    void *context,
    dtv_l3_provider_binding *binding,
    uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->release_claim_calls;
    if (fixture->reentry_mode == 3 && fixture->owner != NULL) {
        fixture->inner_close = dtv_l3_close(fixture->owner, 100);
        fixture->inner_release = dtv_l3_context_release(fixture->owner);
    }
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
    (void)request;
    fixture->complete = complete;
    fixture->complete_context = callback_context;
    fixture->complete_binding = binding;
    fixture->complete_id = transfer_id;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result provider_cancel(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint64_t transfer_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)transfer_id;
    ++fixture->cancel_calls;
    if (fixture->reentry_mode == 2 && fixture->owner != NULL) {
        fixture->inner_close = dtv_l3_close(fixture->owner, 100);
        fixture->inner_wait = dtv_l3_op_wait(fixture->op, 100);
        fixture->inner_release = dtv_l3_context_release(fixture->owner);
    }
    while (atomic_load(&fixture->hold_cancel)) {
        sched_yield();
    }
    if (fixture->complete_on_cancel && fixture->complete != NULL) {
        const dtv_l3_transfer_completion completion = {DTV_L3_CANCELLED, NULL,
                                                       0};
        fixture->complete(fixture->complete_binding, fixture->complete_id,
                          &completion, fixture->complete_context);
    }
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result provider_scope(void *context,
                                    uint32_t receiver_id,
                                    dtv_l3_scope_info *scope) {
    (void)context;
    (void)receiver_id;
    scope->bridge_id = 7;
    scope->compatibility = DTV_L3_LEASE_SHAREABLE;
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
    ++fixture->start_calls;
    return DTV_L3_OK;
}

static dtv_l3_result provider_bridge_stop(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t bridge_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)bridge_id;
    ++fixture->stop_calls;
    return DTV_L3_OK;
}

static dtv_l3_result provider_stream_attach(void *context,
                                            dtv_l3_provider_binding *binding,
                                            uint64_t stream_id,
                                            dtv_l3_ts_deliver_fn deliver,
                                            void *callback_context) {
    struct fixture *fixture = context;
    (void)binding;
    ++fixture->attach_calls;
    if (stream_id >= MAX_STREAMS) {
        return DTV_L3_INVALID;
    }
    fixture->deliver[stream_id] = deliver;
    fixture->deliver_context[stream_id] = callback_context;
    return DTV_L3_OK;
}

static dtv_l3_result provider_stream_stop(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint64_t stream_id) {
    struct fixture *fixture = context;
    (void)binding;
    ++fixture->stream_stop_calls;
    if (stream_id < MAX_STREAMS && fixture->deliver[stream_id] != NULL) {
        fixture->deliver[stream_id](fixture->deliver_context[stream_id], NULL,
                                    0);
    }
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

static struct fixture fixture_new(void) {
    struct fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    atomic_init(&fixture.hold_claim, 0);
    atomic_init(&fixture.claim_entered, 0);
    atomic_init(&fixture.allow_claim, 0);
    atomic_init(&fixture.hold_cancel, 0);
    atomic_init(&fixture.claim_cancel_called, 0);
    return fixture;
}

static dtv_l3_provider provider_for(struct fixture *fixture) {
    return (dtv_l3_provider){.provider_context = fixture,
                             .claim = provider_claim,
                             .release_claim = provider_release_claim,
                             .submit = provider_submit,
                             .cancel = provider_cancel,
                             .scope_query = provider_scope,
                             .bridge_start = provider_bridge_start,
                             .bridge_stop = provider_bridge_stop,
                             .stream_attach = provider_stream_attach,
                             .stream_stop = provider_stream_stop,
                             .tune = provider_tune,
                             .claim_cancel = fixture->null_claim_cancel
                                                 ? NULL
                                                 : provider_claim_cancel};
}

static int wait_value(atomic_int *value, int expected, unsigned int attempts) {
    unsigned int attempt;
    for (attempt = 0; attempt < attempts; ++attempt) {
        if (atomic_load(value) == expected) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

static int wait_close_flags(dtv_l3_context *owner,
                            dtv_l3_close_status *status,
                            int wait_releasable) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (dtv_l3_close_status_get(owner, status) != DTV_L3_OK) {
            return 0;
        }
        if (status->physical_quiesced && status->cleanup_complete &&
            status->all_ops_recovered && status->control_lane == DTV_L3_LANE_EXITED &&
            status->open_lane == DTV_L3_LANE_EXITED &&
            (!wait_releasable || status->releasable)) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

static int wait_open_lane_exited(dtv_l3_context *owner) {
    unsigned int attempt;
    dtv_l3_close_status status;
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (dtv_l3_close_status_get(owner, &status) != DTV_L3_OK) {
            return 0;
        }
        if (status.open_lane == DTV_L3_LANE_EXITED) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
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

static dtv_l3_context *open_ready(dtv_l3_provider *provider,
                                  dtv_l3_op **op_out) {
    dtv_l3_open_result opened = dtv_l3_open(provider, 1);
    dtv_l3_op_result result;
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return NULL;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK) {
        return NULL;
    }
    if (op_out != NULL) {
        *op_out = opened.op;
    } else if (dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return NULL;
    }
    return opened.owner;
}

static long long now_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (long long)value.tv_sec * 1000LL + value.tv_nsec / 1000000LL;
}

static int provider_fire(struct fixture *fixture,
                         dtv_l3_result result,
                         const uint8_t *data,
                         size_t size) {
    if (fixture->complete == NULL) {
        return 0;
    }
    const dtv_l3_transfer_completion completion = {result, data, size};
    fixture->complete(fixture->complete_binding, fixture->complete_id,
                      &completion, fixture->complete_context);
    return 1;
}

static dtv_l3_context *open_ready_poll(dtv_l3_provider *provider) {
    dtv_l3_open_result opened = dtv_l3_open(provider, 1);
    dtv_l3_op_result result;
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return NULL;
    }
    if (!wait_op_terminal(opened.op, &result) ||
        result.result != DTV_L3_OK || dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return NULL;
    }
    return opened.owner;
}

struct wait_call {
    dtv_l3_op *op;
    dtv_l3_stream *stream;
    uint32_t timeout_ms;
    uint8_t *buffer;
    size_t capacity;
    dtv_l3_result result;
    size_t bytes_read;
    dtv_l3_read_state state;
    atomic_int done;
};

static void *run_op_wait(void *argument) {
    struct wait_call *call = argument;
    call->result = dtv_l3_op_wait(call->op, call->timeout_ms);
    atomic_store(&call->done, 1);
    return NULL;
}

static void *run_read_wait(void *argument) {
    struct wait_call *call = argument;
    call->result =
        dtv_l3_stream_read_ts_wait(call->stream, call->buffer, call->capacity,
                                   &call->bytes_read, &call->state,
                                   call->timeout_ms);
    atomic_store(&call->done, 1);
    return NULL;
}

static int test_blocking_claim_wait_close(void) {
    struct fixture fixture = fixture_new();
    struct wait_call call;
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_close_status status;
    dtv_l3_op_result result;
    pthread_t waiter;
    int before;
    int during;

    atomic_store(&fixture.hold_claim, 1);
    before = count_tasks();
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (!wait_value(&fixture.claim_entered, 1, 2000) ||
        dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK ||
        status.open_lane != DTV_L3_LANE_RUNNING ||
        status.control_lane != DTV_L3_LANE_RUNNING || status.live_lanes != 2) {
        return 2;
    }
    fixture.owner = opened.owner;
    fixture.op = opened.op;
    fixture.reentry_mode = 1;
    memset(&call, 0, sizeof(call));
    atomic_init(&call.done, 0);
    call.op = opened.op;
    call.timeout_ms = DTV_L3_WAIT_FOREVER;
    if (pthread_create(&waiter, NULL, run_op_wait, &call) != 0) {
        return 3;
    }
    sleep_ms(50);
    if (dtv_l3_close(opened.owner, 200) != DTV_L3_TIMEOUT) {
        return 4;
    }
    if (fixture.claim_cancel_calls != 1 || fixture.claim_cancel_index != 0 ||
        fixture.claim_cancel_binding != fixture.claim_binding ||
        fixture.release_claim_calls != 0 || fixture.cancel_calls != 0) {
        return 4;
    }
    if (!wait_value(&call.done, 1, 2000) || call.result != DTV_L3_CLOSING) {
        return 5;
    }
    if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK ||
        status.control_lane == DTV_L3_LANE_EXITED) {
        return 6;
    }
    if (dtv_l3_context_release(opened.owner) != DTV_L3_BUSY) {
        return 7;
    }
    atomic_store(&fixture.allow_claim, 1);
    pthread_join(waiter, NULL);
    if (!wait_op_terminal(opened.op, &result) ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 9;
    }
    if (fixture.inner_close != DTV_L3_ACCEPTED ||
        fixture.inner_wait != DTV_L3_CLOSING ||
        fixture.inner_release != DTV_L3_BUSY) {
        return 8;
    }
    if (!wait_close_flags(opened.owner, &status, 1)) {
        return 10;
    }
    if (L3_CHECK_TASK_COUNT && (before < 0 || !wait_task_count(before))) {
        return 11;
    }
    during = count_tasks();
    (void)during;
    if (dtv_l3_context_release(opened.owner) != DTV_L3_OK) {
        return 12;
    }
    return fixture.release_claim_calls == 1 && fixture.claim_cancel_calls == 1
               ? 0
               : 13;
}

static int test_close_once_sequence(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_close_status status;
    dtv_l3_op *op = NULL;
    int before;
    int ready;

    before = count_tasks();
    owner = open_ready(&provider, &op);
    if (owner == NULL || op == NULL) {
        return 1;
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK ||
        !wait_open_lane_exited(owner)) {
        return 2;
    }
    if (L3_CHECK_TASK_COUNT && (before < 0 || !wait_task_count(before + 1))) {
        return 3;
    }
    ready = count_tasks();
    (void)ready;
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK) {
        return 4;
    }
    if (!wait_close_flags(owner, &status, 1)) {
        return 5;
    }
    if (dtv_l3_close_status_get(owner, &status) != DTV_L3_OK ||
        status.open_lane != DTV_L3_LANE_EXITED ||
        status.control_lane != DTV_L3_LANE_EXITED || status.live_lanes != 0) {
        return 6;
    }
    if (L3_CHECK_TASK_COUNT && !wait_task_count(before)) {
        return 7;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 8;
    }
    return fixture.release_claim_calls == 1 && fixture.claim_calls == 1 ? 0 : 9;
}

static int test_open_handle_release_gate(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_op *op = NULL;
    dtv_l3_close_status status;

    owner = open_ready(&provider, &op);
    if (owner == NULL || op == NULL) {
        return 1;
    }
    (void)dtv_l3_close(owner, 0);
    if (!wait_close_flags(owner, &status, 0)) {
        return 2;
    }
    if (status.releasable) {
        return 3;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_BUSY) {
        return 4;
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK) {
        return 5;
    }
    if (!wait_close_flags(owner, &status, 1)) {
        return 6;
    }
    return dtv_l3_context_release(owner) == DTV_L3_OK ? 0 : 7;
}

static int test_webts_close_poll(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_close_status status;
    dtv_l3_result close_result;

    owner = open_ready_poll(&provider);
    if (owner == NULL) {
        return 1;
    }
    close_result = dtv_l3_close(owner, 0);
    if (close_result != DTV_L3_TIMEOUT && close_result != DTV_L3_OK) {
        return 2;
    }
    if (!wait_close_flags(owner, &status, 1)) {
        return 3;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 4;
    }
    return fixture.claim_calls == 1 && fixture.release_claim_calls == 1 ? 0 : 5;
}

static int test_read_wait_and_close_release(void) {
    struct fixture fixture = fixture_new();
    struct wait_call call;
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_close_status close_status;
    dtv_l3_stream_status stream_status;
    uint64_t programmed = 0;
    uint64_t revision = 0;
    uint64_t generation = 0;
    uint8_t packet[188];
    uint8_t buffer[376];
    size_t bytes = 0;
    dtv_l3_read_state state = DTV_L3_READ_WAIT;
    pthread_t reader;
    unsigned int index;

    owner = open_ready(&provider, NULL);
    if (owner == NULL ||
        dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_OK ||
        dtv_l3_lease_tune(lease, 500000000, &programmed, &revision) !=
            DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK) {
        return 1;
    }
    (void)generation;
    if (dtv_l3_stream_read_ts_wait(stream, buffer, sizeof(buffer), &bytes,
                                   &state, 30) != DTV_L3_WAIT_TIMEOUT ||
        state != DTV_L3_READ_WAIT || bytes != 0) {
        return 2;
    }
    if (dtv_l3_stream_read_ts_wait(stream, buffer, 100, &bytes, &state, 10) !=
        DTV_L3_INVALID) {
        return 3;
    }
    for (index = 0; index < sizeof(packet); ++index) {
        packet[index] = (uint8_t)index;
    }
    stream_status = (dtv_l3_stream_status){0};
    if (dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK) {
        return 4;
    }
    generation = stream_status.stream_generation;
    if (generation >= MAX_STREAMS || fixture.deliver[generation] == NULL) {
        return 5;
    }
    fixture.deliver[generation](fixture.deliver_context[generation], packet,
                                sizeof(packet));
    if (dtv_l3_stream_read_ts_wait(stream, buffer, sizeof(buffer), &bytes,
                                   &state, 1000) != DTV_L3_OK ||
        state != DTV_L3_READ_DATA || bytes != sizeof(packet) ||
        buffer[1] != 1) {
        return 6;
    }

    memset(&call, 0, sizeof(call));
    atomic_init(&call.done, 0);
    call.stream = stream;
    call.buffer = buffer;
    call.capacity = sizeof(buffer);
    call.timeout_ms = DTV_L3_WAIT_FOREVER;
    if (pthread_create(&reader, NULL, run_read_wait, &call) != 0) {
        return 7;
    }
    sleep_ms(50);
    (void)dtv_l3_close(owner, 0);
    if (!wait_value(&call.done, 1, 2000) || call.result != DTV_L3_CLOSING) {
        return 8;
    }
    pthread_join(reader, NULL);
    if (!wait_close_flags(owner, &close_status, 0)) {
        return 9;
    }
    if (dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK ||
        stream_status.discarded_unread_packets != 0) {
        return 10;
    }
    if (dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 11;
    }
    if (!wait_close_flags(owner, &close_status, 1)) {
        return 12;
    }
    return dtv_l3_context_release(owner) == DTV_L3_OK ? 0 : 13;
}

static int test_startup_failure_injection(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_context *owner;
    unsigned int attempt;
    int before = count_tasks();

    l3_test_set_lane_start_failure_mask(1);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL || fixture.claim_calls != 0) {
        return 1;
    }
    l3_test_set_lane_start_failure_mask(2);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL || fixture.claim_calls != 0) {
        return 2;
    }
    l3_test_set_lane_start_failure_mask(0);
    owner = open_ready(&provider, NULL);
    if (owner == NULL || count_tasks() < before) {
        return 3;
    }
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK) {
        return 4;
    }
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (dtv_l3_context_release(owner) == DTV_L3_OK) {
            break;
        }
        sleep_ms(1);
    }
    if (attempt == 4000) {
        return 5;
    }
    return !L3_CHECK_TASK_COUNT || wait_task_count(before) ? 0 : 6;
}

static int test_cancel_reentry_frame(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_close_status status;
    dtv_l3_op_result result;
    dtv_l3_op *op = NULL;
    dtv_l3_transfer_request request;
    uint8_t payload[2] = {1, 2};

    owner = open_ready(&provider, NULL);
    if (owner == NULL) {
        return 1;
    }
    memset(&request, 0, sizeof(request));
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81;
    request.claim_index = 0;
    request.out_data = payload;
    request.out_size = sizeof(payload);
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == NULL) {
        return 2;
    }
    fixture.owner = owner;
    fixture.op = op;
    fixture.reentry_mode = 2;
    fixture.complete_on_cancel = 1;
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK) {
        return 3;
    }
    if (fixture.inner_close != DTV_L3_ACCEPTED ||
        fixture.inner_wait != DTV_L3_CLOSING ||
        fixture.inner_release != DTV_L3_BUSY) {
        return 4;
    }
    if (!wait_op_terminal(op, &result) ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_CANCELLED ||
        dtv_l3_op_release(op) != DTV_L3_OK) {
        return 5;
    }
    if (!wait_close_flags(owner, &status, 1)) {
        return 6;
    }
    return dtv_l3_context_release(owner) == DTV_L3_OK ? 0 : 7;
}

static int test_release_claim_reentry_frame(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_close_status status;

    owner = open_ready(&provider, NULL);
    if (owner == NULL) {
        return 1;
    }
    fixture.owner = owner;
    fixture.reentry_mode = 3;
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK) {
        return 2;
    }
    if (fixture.inner_close != DTV_L3_ACCEPTED ||
        fixture.inner_release != DTV_L3_BUSY) {
        return 3;
    }
    if (!wait_close_flags(owner, &status, 1)) {
        return 4;
    }
    return dtv_l3_context_release(owner) == DTV_L3_OK ? 0 : 5;
}

static int test_wait_completion_wakes(void) {
    struct fixture fixture = fixture_new();
    struct wait_call call;
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    uint8_t payload[2] = {3, 4};
    uint8_t response[1] = {0x9a};
    long long started;
    pthread_t waiter;

    owner = open_ready(&provider, NULL);
    if (owner == NULL) {
        return 1;
    }
    memset(&request, 0, sizeof(request));
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81;
    request.claim_index = 0;
    request.out_data = payload;
    request.out_size = sizeof(payload);
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == NULL) {
        return 2;
    }
    memset(&call, 0, sizeof(call));
    atomic_init(&call.done, 0);
    call.op = op;
    call.timeout_ms = 1200;
    if (pthread_create(&waiter, NULL, run_op_wait, &call) != 0) {
        return 3;
    }
    sleep_ms(50);
    started = now_ms();
    if (!provider_fire(&fixture, DTV_L3_TIMEOUT, response,
                       sizeof(response))) {
        return 4;
    }
    if (!wait_value(&call.done, 1, 1500) || call.result != DTV_L3_OK ||
        now_ms() - started > 1000) {
        return 5;
    }
    pthread_join(waiter, NULL);
    if (!wait_op_terminal(op, &result) || result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_TIMEOUT || result.data_size != sizeof(response) ||
        result.data == NULL || result.data[0] != response[0] ||
        dtv_l3_op_release(op) != DTV_L3_OK) {
        return 6;
    }

    op = NULL;
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == NULL) {
        return 7;
    }
    memset(&call, 0, sizeof(call));
    atomic_init(&call.done, 0);
    call.op = op;
    call.timeout_ms = DTV_L3_WAIT_FOREVER;
    if (pthread_create(&waiter, NULL, run_op_wait, &call) != 0) {
        return 8;
    }
    sleep_ms(50);
    started = now_ms();
    if (!provider_fire(&fixture, DTV_L3_OK, response, sizeof(response)) ||
        !wait_value(&call.done, 1, 1500) || call.result != DTV_L3_OK ||
        now_ms() - started > 1000) {
        return 9;
    }
    pthread_join(waiter, NULL);
    if (!wait_op_terminal(op, &result) || result.state != DTV_L3_OP_DONE ||
        result.result != DTV_L3_OK || dtv_l3_op_release(op) != DTV_L3_OK) {
        return 10;
    }

    if (dtv_l3_close(owner, 2000) != DTV_L3_OK ||
        dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 11;
    }
    return 0;
}

static int test_wait_packet_wakes(void) {
    struct fixture fixture = fixture_new();
    struct wait_call call;
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_stream_status stream_status;
    uint64_t programmed = 0;
    uint64_t revision = 0;
    uint64_t generation = 0;
    uint8_t packet[188];
    uint8_t buffer[188];
    size_t bytes = 0;
    dtv_l3_read_state state = DTV_L3_READ_WAIT;
    long long started;
    pthread_t waiter;
    unsigned int index;

    owner = open_ready(&provider, NULL);
    if (owner == NULL ||
        dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_OK ||
        dtv_l3_lease_tune(lease, 500000000, &programmed, &revision) !=
            DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK) {
        return 1;
    }
    for (index = 0; index < sizeof(packet); ++index) {
        packet[index] = (uint8_t)index;
    }
    stream_status = (dtv_l3_stream_status){0};
    if (dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK) {
        return 2;
    }
    generation = stream_status.stream_generation;
    if (generation >= MAX_STREAMS || fixture.deliver[generation] == NULL) {
        return 3;
    }
    memset(&call, 0, sizeof(call));
    atomic_init(&call.done, 0);
    call.stream = stream;
    call.buffer = buffer;
    call.capacity = sizeof(buffer);
    call.timeout_ms = 1200;
    if (pthread_create(&waiter, NULL, run_read_wait, &call) != 0) {
        return 4;
    }
    sleep_ms(50);
    started = now_ms();
    fixture.deliver[generation](fixture.deliver_context[generation], packet,
                                sizeof(packet));
    if (!wait_value(&call.done, 1, 1500) || call.result != DTV_L3_OK ||
        call.state != DTV_L3_READ_DATA || call.bytes_read != sizeof(packet) ||
        now_ms() - started > 1000) {
        return 5;
    }
    pthread_join(waiter, NULL);
    if (dtv_l3_stream_read_ts_wait(stream, buffer, 188, &bytes, &state, 20) !=
        DTV_L3_WAIT_TIMEOUT) {
        return 6;
    }
    (void)dtv_l3_close(owner, 0);
    if (!wait_close_flags(owner, &(dtv_l3_close_status){0}, 0)) {
        return 7;
    }
    if (dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 8;
    }
    {
        dtv_l3_close_status close_status;
        if (!wait_close_flags(owner, &close_status, 1)) {
            return 9;
        }
    }
    return dtv_l3_context_release(owner) == DTV_L3_OK ? 0 : 10;
}

static int test_claim_cancel_return(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_close_status status;
    dtv_l3_op_result result;
    long long started;

    atomic_store(&fixture.hold_claim, 1);
    fixture.claim_cancel_unblocks = 1;
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL || !wait_value(&fixture.claim_entered, 1, 2000)) {
        return 1;
    }
    started = now_ms();
    if (dtv_l3_close(opened.owner, 2000) != DTV_L3_OK ||
        now_ms() - started > 1500) {
        return 2;
    }
    if (fixture.claim_cancel_calls != 1 || fixture.claim_cancel_index != 0 ||
        fixture.claim_cancel_binding != fixture.claim_binding ||
        fixture.claim_binding == NULL || fixture.cancel_calls != 0) {
        return 3;
    }
    if (!wait_op_terminal(opened.op, &result) ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 4;
    }
    if (!wait_close_flags(opened.owner, &status, 1)) {
        return 5;
    }
    if (fixture.release_claim_calls != 1) {
        return 6;
    }
    return dtv_l3_context_release(opened.owner) == DTV_L3_OK ? 0 : 7;
}

static int test_claim_cancel_unsupported(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_close_status status;
    dtv_l3_op_result result;

    atomic_store(&fixture.hold_claim, 1);
    fixture.null_claim_cancel = 1;
    provider = provider_for(&fixture);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL || !wait_value(&fixture.claim_entered, 1, 2000)) {
        return 1;
    }
    if (dtv_l3_close(opened.owner, 200) != DTV_L3_TIMEOUT) {
        return 2;
    }
    if (fixture.claim_cancel_calls != 0 || fixture.cancel_calls != 0 ||
        dtv_l3_context_release(opened.owner) != DTV_L3_BUSY) {
        return 3;
    }
    if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK ||
        status.open_lane != DTV_L3_LANE_RUNNING ||
        status.control_lane != DTV_L3_LANE_RUNNING) {
        return 4;
    }
    atomic_store(&fixture.allow_claim, 1);
    if (!wait_op_terminal(opened.op, &result) ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK ||
        !wait_close_flags(opened.owner, &status, 1)) {
        return 5;
    }
    return dtv_l3_context_release(opened.owner) == DTV_L3_OK ? 0 : 6;
}

static int test_claim_cancel_after_ready(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_context *owner;

    owner = open_ready(&provider, NULL);
    if (owner == NULL) {
        return 1;
    }
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK) {
        return 2;
    }
    if (fixture.claim_cancel_calls != 0 || fixture.cancel_calls != 0) {
        return 3;
    }
    return dtv_l3_context_release(owner) == DTV_L3_OK ? 0 : 4;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 64;
    }
    if (strcmp(argv[1], "open-wait-close") == 0) {
        return test_blocking_claim_wait_close();
    }
    if (strcmp(argv[1], "close-once") == 0) {
        return test_close_once_sequence();
    }
    if (strcmp(argv[1], "release-gate") == 0) {
        return test_open_handle_release_gate();
    }
    if (strcmp(argv[1], "close-poll") == 0) {
        return test_webts_close_poll();
    }
    if (strcmp(argv[1], "read-wait") == 0) {
        return test_read_wait_and_close_release();
    }
    if (strcmp(argv[1], "startup-fail") == 0) {
        return test_startup_failure_injection();
    }
    if (strcmp(argv[1], "cancel-reentry") == 0) {
        return test_cancel_reentry_frame();
    }
    if (strcmp(argv[1], "cleanup-reentry") == 0) {
        return test_release_claim_reentry_frame();
    }
    if (strcmp(argv[1], "wait-completion") == 0) {
        return test_wait_completion_wakes();
    }
    if (strcmp(argv[1], "wait-packet") == 0) {
        return test_wait_packet_wakes();
    }
    if (strcmp(argv[1], "claim-cancel-return") == 0) {
        return test_claim_cancel_return();
    }
    if (strcmp(argv[1], "claim-cancel-unsupported") == 0) {
        return test_claim_cancel_unsupported();
    }
    if (strcmp(argv[1], "claim-cancel-after-ready") == 0) {
        return test_claim_cancel_after_ready();
    }
    return 65;
}
