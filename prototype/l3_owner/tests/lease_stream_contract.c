/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/l3_owner.h>

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_SCOPES 8
#define MAX_BRIDGES 16
#define MAX_STREAMS 64

struct fixture {
    dtv_l3_scope_info scopes[MAX_SCOPES];
    uint32_t scope_receiver[MAX_SCOPES];
    unsigned int scope_count;
    dtv_l3_result start_result[MAX_BRIDGES];
    dtv_l3_result bridge_stop_result[MAX_BRIDGES];
    unsigned int start_calls[MAX_BRIDGES];
    unsigned int stop_calls[MAX_BRIDGES];
    dtv_l3_result attach_result;
    dtv_l3_result stream_stop_result;
    unsigned int attach_calls;
    unsigned int stream_stop_calls;
    unsigned int tune_calls;
    uint64_t last_tune_hz;
    dtv_l3_ts_deliver_fn deliver[MAX_STREAMS];
    void *deliver_context[MAX_STREAMS];
};

static dtv_l3_claim_result provider_claim(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t claim_index) {
    (void)context;
    (void)binding;
    (void)claim_index;
    return (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

static dtv_l3_release_result provider_release_claim(
    void *context,
    dtv_l3_provider_binding *binding,
    uint32_t claim_index) {
    (void)context;
    (void)binding;
    (void)claim_index;
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
    (void)context;
    (void)binding;
    (void)transfer_id;
    (void)request;
    (void)complete;
    (void)callback_context;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result provider_cancel(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint64_t transfer_id) {
    (void)context;
    (void)binding;
    (void)transfer_id;
    return DTV_L3_OK;
}

static dtv_l3_result provider_scope(void *context,
                                    uint32_t receiver_id,
                                    dtv_l3_scope_info *scope) {
    struct fixture *fixture = context;
    unsigned int index;
    for (index = 0; index < fixture->scope_count; ++index) {
        if (fixture->scope_receiver[index] == receiver_id) {
            *scope = fixture->scopes[index];
            return DTV_L3_OK;
        }
    }
    return DTV_L3_UNSUPPORTED;
}

static dtv_l3_result provider_bridge_start(void *context,
                                           dtv_l3_provider_binding *binding,
                                           uint32_t bridge_id,
                                           uint32_t receiver_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)receiver_id;
    if (bridge_id >= MAX_BRIDGES) {
        return DTV_L3_INVALID;
    }
    ++fixture->start_calls[bridge_id];
    return fixture->start_result[bridge_id];
}

static dtv_l3_result provider_bridge_stop(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t bridge_id) {
    struct fixture *fixture = context;
    (void)binding;
    if (bridge_id >= MAX_BRIDGES) {
        return DTV_L3_INVALID;
    }
    ++fixture->stop_calls[bridge_id];
    return fixture->bridge_stop_result[bridge_id];
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
    return fixture->attach_result;
}

static dtv_l3_result provider_stream_stop(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint64_t stream_id) {
    struct fixture *fixture = context;
    (void)binding;
    ++fixture->stream_stop_calls;
    if (fixture->stream_stop_result == DTV_L3_OK &&
        stream_id < MAX_STREAMS && fixture->deliver[stream_id] != NULL) {
        fixture->deliver[stream_id](fixture->deliver_context[stream_id], NULL,
                                    0);
    }
    return fixture->stream_stop_result;
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
    fixture->last_tune_hz = requested_hz;
    *programmed_hz = requested_hz + 1000;
    return DTV_L3_OK;
}

static struct fixture fixture_new(void) {
    struct fixture fixture;
    unsigned int index;
    memset(&fixture, 0, sizeof(fixture));
    fixture.attach_result = DTV_L3_OK;
    fixture.stream_stop_result = DTV_L3_OK;
    for (index = 0; index < MAX_BRIDGES; ++index) {
        fixture.start_result[index] = DTV_L3_OK;
        fixture.bridge_stop_result[index] = DTV_L3_OK;
    }
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
                             .tune = provider_tune};
}

static void add_scope(struct fixture *fixture,
                      uint32_t receiver_id,
                      uint32_t bridge_id,
                      uint32_t compatibility) {
    fixture->scope_receiver[fixture->scope_count] = receiver_id;
    fixture->scopes[fixture->scope_count].bridge_id = bridge_id;
    fixture->scopes[fixture->scope_count].compatibility = compatibility;
    ++fixture->scope_count;
}

static int wait_open(dtv_l3_op *op, dtv_l3_op_result *result) {
    unsigned int attempt;
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_op_poll(op, result) != DTV_L3_OK) {
            return 0;
        }
        if (result->state == DTV_L3_OP_DONE ||
            result->state == DTV_L3_OP_FAILED) {
            return 1;
        }
        sched_yield();
    }
    return 0;
}

static dtv_l3_context *open_ready(dtv_l3_provider *provider) {
    dtv_l3_open_result opened = dtv_l3_open(provider, 1);
    dtv_l3_op_result result;
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL || !wait_open(opened.op, &result) ||
        result.result != DTV_L3_OK || dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return NULL;
    }
    return opened.owner;
}

static int close_ready(dtv_l3_context *owner) {
    dtv_l3_close_status status;
    unsigned int attempt;
    dtv_l3_result result = DTV_L3_BUSY;
    if (dtv_l3_close(owner, 1000) != DTV_L3_OK) {
        return 0;
    }
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(owner, &status) != DTV_L3_OK) {
            return 0;
        }
        if (status.cleanup_complete && status.all_ops_recovered) {
            break;
        }
        sched_yield();
    }
    if (attempt == 1000000) {
        return 0;
    }
    for (attempt = 0; attempt < 1000000 && result == DTV_L3_BUSY; ++attempt) {
        result = dtv_l3_context_release(owner);
        sched_yield();
    }
    return result == DTV_L3_OK;
}

static uint64_t stream_generation(dtv_l3_stream *stream) {
    dtv_l3_stream_status status;
    if (stream == NULL ||
        dtv_l3_stream_status_get(stream, &status) != DTV_L3_OK) {
        return 0;
    }
    return status.stream_generation;
}

static uint64_t arm_stream(struct fixture *fixture,
                           dtv_l3_context *owner,
                           uint32_t receiver_id,
                           dtv_l3_lease **lease_out,
                           dtv_l3_stream **stream_out) {
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    uint64_t programmed = 0;
    uint64_t revision = 0;
    if (dtv_l3_lease_acquire(owner, receiver_id, &lease) != DTV_L3_OK ||
        lease == NULL ||
        dtv_l3_lease_tune(lease, 500000000, &programmed, &revision) !=
            DTV_L3_OK ||
        programmed != 500001000 || revision != 1 ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK || stream == NULL) {
        return 0;
    }
    (void)fixture;
    *lease_out = lease;
    *stream_out = stream;
    return 1;
}

static void fill_packet(uint8_t *packet, uint8_t seed) {
    unsigned int index;
    for (index = 0; index < 188; ++index) {
        packet[index] = (uint8_t)(seed + index);
    }
}

static void feed_short(struct fixture *fixture,
                       uint64_t generation,
                       uint8_t seed) {
    uint8_t packet[188];
    if (generation >= MAX_STREAMS || fixture->deliver[generation] == NULL) {
        return;
    }
    fill_packet(packet, seed);
    fixture->deliver[generation](fixture->deliver_context[generation], packet,
                                 100);
    fixture->deliver[generation](fixture->deliver_context[generation],
                                 packet + 100, 88);
}

static void feed_one(struct fixture *fixture, uint64_t generation) {
    uint8_t packet[188];
    if (generation >= MAX_STREAMS || fixture->deliver[generation] == NULL) {
        return;
    }
    fill_packet(packet, 0x40);
    fixture->deliver[generation](fixture->deliver_context[generation], packet,
                                 sizeof(packet));
}

static int drain_and_finish(dtv_l3_stream *stream) {
    uint8_t packet[188];
    size_t bytes = 0;
    dtv_l3_read_state state;
    dtv_l3_result result = DTV_L3_OK;
    unsigned int attempt;
    for (attempt = 0; attempt < 1000000; ++attempt) {
        result = dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes,
                                       &state);
        if (result != DTV_L3_OK || state == DTV_L3_READ_EOF) {
            break;
        }
        if (state == DTV_L3_READ_WAIT) {
            sched_yield();
        }
    }
    if (result != DTV_L3_OK || state != DTV_L3_READ_EOF) {
        return 0;
    }
    return dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DRAINED) ==
                   DTV_L3_OK &&
           dtv_l3_stream_destroy(stream) == DTV_L3_OK;
}

static int finish_stream(dtv_l3_stream *stream) {
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK) {
        return 0;
    }
    return drain_and_finish(stream);
}

static int test_shareable_and_exclusive(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease0 = NULL;
    dtv_l3_lease *lease1 = NULL;
    dtv_l3_lease *lease2 = NULL;
    dtv_l3_lease *lease3 = NULL;
    dtv_l3_lease *lease4 = NULL;
    dtv_l3_lease *lease5 = NULL;
    dtv_l3_stream *stream0 = NULL;
    dtv_l3_stream *stream1 = NULL;
    dtv_l3_lease_status status;
    uint8_t packet[188];
    size_t bytes = 0;
    dtv_l3_read_state state;
    int failed = 0;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    add_scope(&fixture, 1, 7, DTV_L3_LEASE_SHAREABLE);
    add_scope(&fixture, 2, 8, DTV_L3_LEASE_EXCLUSIVE);
    add_scope(&fixture, 3, 8, DTV_L3_LEASE_EXCLUSIVE);
    add_scope(&fixture, 4, 9, DTV_L3_LEASE_EXCLUSIVE);
    add_scope(&fixture, 5, 9, DTV_L3_LEASE_SHAREABLE);
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (!arm_stream(&fixture, owner, 0, &lease0, &stream0) ||
        !arm_stream(&fixture, owner, 1, &lease1, &stream1)) {
        return 2;
    }
    if (fixture.start_calls[7] != 1 ||
        dtv_l3_lease_status_get(lease0, &status) != DTV_L3_OK ||
        status.bridge_active_users != 2 || status.state != DTV_L3_LEASE_ACTIVE) {
        failed = 3;
    }
    if (dtv_l3_lease_acquire(owner, 0, &lease2) != DTV_L3_BUSY ||
        dtv_l3_lease_acquire(owner, 2, &lease2) != DTV_L3_OK ||
        dtv_l3_lease_acquire(owner, 3, &lease3) != DTV_L3_BUSY) {
        failed = 4;
    }
    if (dtv_l3_lease_acquire(owner, 4, &lease4) != DTV_L3_OK ||
        dtv_l3_lease_acquire(owner, 5, &lease5) != DTV_L3_BUSY) {
        failed = 5;
    }
    if (dtv_l3_lease_release(lease2) != DTV_L3_OK ||
        dtv_l3_lease_release(lease4) != DTV_L3_OK) {
        failed = 5;
    }
    feed_one(&fixture, stream_generation(stream1));
    if (dtv_l3_stream_stop(stream0, 0) != DTV_L3_OK ||
        fixture.stop_calls[7] != 0 ||
        dtv_l3_lease_status_get(lease0, &status) != DTV_L3_OK ||
        status.bridge_active_users != 1 ||
        dtv_l3_stream_stop(stream0, 0) != DTV_L3_OK ||
        fixture.stop_calls[7] != 0 ||
        dtv_l3_lease_status_get(lease0, &status) != DTV_L3_OK ||
        status.bridge_active_users != 1) {
        failed = 6;
    }
    if (dtv_l3_stream_read_ts(stream1, packet, sizeof(packet), &bytes,
                              &state) != DTV_L3_OK ||
        state != DTV_L3_READ_DATA || bytes != 188) {
        failed = 7;
    }
    if (dtv_l3_stream_stop(stream1, 0) != DTV_L3_OK ||
        fixture.stop_calls[7] != 1 ||
        dtv_l3_lease_status_get(lease1, &status) != DTV_L3_OK ||
        status.bridge_active_users != 0) {
        failed = 8;
    }
    if (drain_and_finish(stream0) == 0 || drain_and_finish(stream1) == 0) {
        failed = 9;
    }
    if (!failed && (dtv_l3_lease_release(lease0) != DTV_L3_OK ||
                    dtv_l3_lease_release(lease1) != DTV_L3_OK ||
                    !close_ready(owner))) {
        failed = 10;
    }
    return failed;
}

static int test_tuning_and_generations(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_lease_status lease_status;
    dtv_l3_stream_status stream_status;
    uint64_t programmed = 0;
    uint64_t revision = 0;
    uint64_t first_generation;
    uint64_t second_generation = 0;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (!arm_stream(&fixture, owner, 0, &lease, &stream)) {
        return 2;
    }
    first_generation = stream_generation(stream);
    if (dtv_l3_lease_status_get(lease, &lease_status) != DTV_L3_OK ||
        lease_status.state != DTV_L3_LEASE_ACTIVE ||
        dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK ||
        stream_status.tune_revision != 1 ||
        dtv_l3_lease_tune(lease, 600000000, &programmed, &revision) !=
            DTV_L3_BUSY ||
        dtv_l3_lease_start(lease) != DTV_L3_BUSY) {
        return 3;
    }
    if (finish_stream(stream) == 0 ||
        dtv_l3_lease_status_get(lease, &lease_status) != DTV_L3_OK ||
        lease_status.state != DTV_L3_LEASE_FREE) {
        return 4;
    }
    if (dtv_l3_lease_tune(lease, 600000000, &programmed, &revision) !=
            DTV_L3_OK ||
        programmed != 600001000 || revision != 2 ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK || stream == NULL) {
        return 5;
    }
    second_generation = stream_generation(stream);
    if (second_generation <= first_generation ||
        dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK ||
        stream_status.tune_revision != 2) {
        return 6;
    }
    if (finish_stream(stream) == 0 || dtv_l3_lease_release(lease) != DTV_L3_OK ||
        !close_ready(owner)) {
        return 7;
    }
    return 0;
}

static int test_ts_boundary_backpressure_and_eof(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_stream_status status;
    uint8_t packet[4 * 188];
    uint8_t one[188];
    size_t bytes = 0;
    dtv_l3_read_state state;
    uint8_t seed;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (!arm_stream(&fixture, owner, 0, &lease, &stream)) {
        return 2;
    }
    for (seed = 0; seed < 6; ++seed) {
        feed_short(&fixture, stream_generation(stream), (uint8_t)(seed * 3));
    }
    if (dtv_l3_stream_status_get(stream, &status) != DTV_L3_OK ||
        status.queued_packets != 4 || status.dropped_overflow_packets != 2 ||
        status.staged_bytes != 0) {
        return 3;
    }
    if (dtv_l3_stream_read_ts(stream, packet, 100, &bytes, &state) !=
        DTV_L3_INVALID) {
        return 4;
    }
    if (dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_DATA || bytes != sizeof(packet) ||
        packet[0] != 0 || packet[188] != 3) {
        return 5;
    }
    if (dtv_l3_stream_read_ts(stream, one, sizeof(one), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_WAIT || bytes != 0) {
        return 6;
    }
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK ||
        dtv_l3_stream_status_get(stream, &status) != DTV_L3_OK ||
        status.phase != DTV_L3_STREAM_DRAINING ||
        dtv_l3_stream_read_ts(stream, one, sizeof(one), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_EOF ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DRAINED) !=
            DTV_L3_OK ||
        dtv_l3_stream_status_get(stream, &status) != DTV_L3_OK ||
        status.phase != DTV_L3_STREAM_RETIRED ||
        status.dropped_overflow_packets != 2 ||
        status.discarded_unread_packets != 0) {
        return 7;
    }
    if (dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK || !close_ready(owner)) {
        return 8;
    }
    return 0;
}

static int test_drained_requires_eof_and_reacquire(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_stream *next = NULL;
    dtv_l3_lease_status lease_status;
    dtv_l3_stream_status status;
    uint8_t packet[188];
    size_t bytes = 0;
    dtv_l3_read_state state;
    uint64_t first_generation;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (!arm_stream(&fixture, owner, 0, &lease, &stream)) {
        return 2;
    }
    first_generation = stream_generation(stream);
    feed_short(&fixture, first_generation, 1);
    feed_short(&fixture, first_generation, 2);
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DRAINED) !=
            DTV_L3_BUSY ||
        dtv_l3_stream_status_get(stream, &status) != DTV_L3_OK ||
        status.phase != DTV_L3_STREAM_DRAINING ||
        status.queued_packets != 2) {
        return 3;
    }
    if (dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_DATA ||
        dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_DATA ||
        dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_EOF ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DRAINED) !=
            DTV_L3_OK ||
        dtv_l3_lease_status_get(lease, &lease_status) != DTV_L3_OK ||
        lease_status.state != DTV_L3_LEASE_FREE) {
        return 4;
    }
    if (dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes, &state) !=
        DTV_L3_STALE) {
        return 5;
    }
    if (dtv_l3_lease_tune(lease, 600000000, &(uint64_t){0},
                          &(uint64_t){0}) != DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &next) != DTV_L3_OK || next == NULL ||
        stream_generation(next) <= first_generation) {
        return 6;
    }
    feed_one(&fixture, stream_generation(next));
    if (dtv_l3_stream_read_ts(next, packet, sizeof(packet), &bytes, &state) !=
            DTV_L3_OK ||
        state != DTV_L3_READ_DATA || packet[0] != 0x40 ||
        dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes, &state) !=
            DTV_L3_STALE) {
        return 7;
    }
    if (dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        finish_stream(next) == 0 ||
        dtv_l3_lease_release(lease) != DTV_L3_OK || !close_ready(owner)) {
        return 8;
    }
    return 0;
}

static int test_discard_unread_and_no_queue_accumulation(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_stream_status status;
    uint8_t packet[188];
    size_t bytes = 0;
    dtv_l3_read_state state;
    unsigned int round;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    for (round = 0; round < 4; ++round) {
        uint64_t programmed = 0;
        uint64_t revision = 0;
        if (dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_OK ||
            dtv_l3_lease_tune(lease, 500000000, &programmed, &revision) !=
                DTV_L3_OK ||
            dtv_l3_lease_start(lease) != DTV_L3_OK ||
            dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK) {
            return 2;
        }
        feed_short(&fixture, stream_generation(stream), 1);
        feed_short(&fixture, stream_generation(stream), 2);
        feed_short(&fixture, stream_generation(stream), 3);
        if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK ||
            dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
                DTV_L3_OK ||
            dtv_l3_stream_status_get(stream, &status) != DTV_L3_OK ||
            status.phase != DTV_L3_STREAM_RETIRED ||
            status.queued_packets != 0 ||
            status.discarded_unread_packets != 3 ||
            dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes,
                                  &state) != DTV_L3_STALE ||
            dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
            dtv_l3_lease_release(lease) != DTV_L3_OK) {
            return 3;
        }
    }
    if (!close_ready(owner)) {
        return 4;
    }
    return 0;
}

static int test_unsupported_card_and_lnb(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_reader *reader = NULL;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (dtv_l3_reader_acquire(owner, 0, &reader) != DTV_L3_UNSUPPORTED ||
        reader != NULL ||
        dtv_l3_lnb_set(owner, 0, 1) != DTV_L3_UNSUPPORTED ||
        fixture.attach_calls != 0 || fixture.tune_calls != 0 ||
        fixture.start_calls[7] != 0 || fixture.stop_calls[7] != 0) {
        return 2;
    }
    return close_ready(owner) ? 0 : 3;
}

static int test_failed_attach_stops_bridge_without_user(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_lease_status status;
    uint64_t programmed = 0;
    uint64_t revision = 0;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    fixture.attach_result = DTV_L3_FAILED;
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_OK ||
        dtv_l3_lease_tune(lease, 500000000, &programmed, &revision) !=
            DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_FAILED ||
        stream != NULL ||
        dtv_l3_lease_status_get(lease, &status) != DTV_L3_OK ||
        status.state != DTV_L3_LEASE_FREE ||
        status.bridge_active_users != 0 || status.stop_residual != 0 ||
        fixture.start_calls[7] != 1 || fixture.stop_calls[7] != 1) {
        return 2;
    }
    fixture.attach_result = DTV_L3_OK;
    if (dtv_l3_lease_start(lease) != DTV_L3_BUSY ||
        dtv_l3_lease_tune(lease, 500000000, &programmed, &revision) !=
            DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK || stream == NULL ||
        fixture.start_calls[7] != 2) {
        return 3;
    }
    if (finish_stream(stream) == 0 ||
        dtv_l3_lease_release(lease) != DTV_L3_OK || !close_ready(owner)) {
        return 4;
    }
    return 0;
}

static int test_stop_residual_retained_then_retried(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_lease_status status;
    dtv_l3_stream_status stream_status;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    fixture.bridge_stop_result[7] = DTV_L3_FAILED;
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (!arm_stream(&fixture, owner, 0, &lease, &stream)) {
        return 2;
    }
    feed_short(&fixture, stream_generation(stream), 1);
    feed_short(&fixture, stream_generation(stream), 2);
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_FAILED ||
        dtv_l3_lease_status_get(lease, &status) != DTV_L3_OK ||
        status.stop_residual == 0 ||
        status.state != DTV_L3_LEASE_RETAINED ||
        dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK ||
        stream_status.queued_packets != 2 ||
        dtv_l3_stream_release(stream,
                              DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
            DTV_L3_OK ||
        dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_BUSY) {
        return 3;
    }
    fixture.bridge_stop_result[7] = DTV_L3_OK;
    if (dtv_l3_lease_release(lease) != DTV_L3_OK || !close_ready(owner)) {
        return 4;
    }
    return 0;
}

static int test_close_retires_active_stream(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_context *owner;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_lease *late = NULL;
    dtv_l3_close_status close_status;
    dtv_l3_lease_status lease_status;
    dtv_l3_stream_status stream_status;
    uint8_t packet[188];
    size_t bytes = 0;
    dtv_l3_read_state state;

    add_scope(&fixture, 0, 7, DTV_L3_LEASE_SHAREABLE);
    provider = provider_for(&fixture);
    owner = open_ready(&provider);
    if (owner == NULL) {
        return 1;
    }
    if (!arm_stream(&fixture, owner, 0, &lease, &stream)) {
        return 2;
    }
    feed_short(&fixture, stream_generation(stream), 1);
    feed_short(&fixture, stream_generation(stream), 2);
    if (dtv_l3_close(owner, 1000) != DTV_L3_OK) {
        return 3;
    }
    if (dtv_l3_close_status_get(owner, &close_status) != DTV_L3_OK ||
        !close_status.cleanup_complete || !close_status.all_ops_recovered) {
        return 4;
    }
    if (fixture.stream_stop_calls != 1) {
        return 5;
    }
    if (fixture.stop_calls[7] != 1) {
        return 6;
    }
    if (dtv_l3_lease_acquire(owner, 0, &late) != DTV_L3_BUSY) {
        return 7;
    }
    if (dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK ||
        stream_status.phase != DTV_L3_STREAM_RETIRED) {
        return 8;
    }
    if (stream_status.discarded_unread_packets != 2) {
        return 9;
    }
    if (dtv_l3_stream_read_ts(stream, packet, sizeof(packet), &bytes, &state) !=
        DTV_L3_STALE) {
        return 10;
    }
    if (dtv_l3_lease_status_get(lease, &lease_status) != DTV_L3_OK ||
        lease_status.state != DTV_L3_LEASE_FREE) {
        return 11;
    }
    if (dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK ||
        dtv_l3_context_release(owner) != DTV_L3_OK) {
        return 4;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 64;
    }
    if (strcmp(argv[1], "share") == 0) {
        return test_shareable_and_exclusive();
    }
    if (strcmp(argv[1], "tuning") == 0) {
        return test_tuning_and_generations();
    }
    if (strcmp(argv[1], "ts") == 0) {
        return test_ts_boundary_backpressure_and_eof();
    }
    if (strcmp(argv[1], "drain") == 0) {
        return test_drained_requires_eof_and_reacquire();
    }
    if (strcmp(argv[1], "discard") == 0) {
        return test_discard_unread_and_no_queue_accumulation();
    }
    if (strcmp(argv[1], "unsupported") == 0) {
        return test_unsupported_card_and_lnb();
    }
    if (strcmp(argv[1], "attach-fail") == 0) {
        return test_failed_attach_stops_bridge_without_user();
    }
    if (strcmp(argv[1], "residual") == 0) {
        return test_stop_residual_retained_then_retried();
    }
    if (strcmp(argv[1], "close") == 0) {
        return test_close_retires_active_stream();
    }
    return 64;
}
