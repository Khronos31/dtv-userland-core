/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Experimental prototype; names and layouts are not a stable ABI promise. */
#ifndef DTV_L3_OWNER_H
#define DTV_L3_OWNER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtv_l3_context dtv_l3_context;
typedef struct dtv_l3_op dtv_l3_op;
typedef struct dtv_l3_provider_binding dtv_l3_provider_binding;
typedef struct dtv_l3_lease dtv_l3_lease;
typedef struct dtv_l3_stream dtv_l3_stream;
typedef struct dtv_l3_reader dtv_l3_reader;

typedef enum dtv_l3_result {
    DTV_L3_OK = 0,
    DTV_L3_ACCEPTED = 1,
    DTV_L3_BUSY = 2,
    DTV_L3_TIMEOUT = 3,
    DTV_L3_CANCELLED = 4,
    DTV_L3_UNSUPPORTED = 5,
    DTV_L3_INVALID = 6,
    DTV_L3_FAILED = 7,
    DTV_L3_STALE = 8,
    DTV_L3_WAIT_TIMEOUT = 9,
    DTV_L3_CLOSING = 10
} dtv_l3_result;

/* A wait deadline of zero observes immediately; this value waits indefinitely. */
#define DTV_L3_WAIT_FOREVER 0xffffffffu

typedef enum dtv_l3_op_state {
    DTV_L3_OP_OPENING = 0,
    DTV_L3_OP_RUNNING = 1,
    DTV_L3_OP_DONE = 2,
    DTV_L3_OP_FAILED = 3
} dtv_l3_op_state;

typedef enum dtv_l3_op_kind {
    DTV_L3_OP_KIND_OPEN = 1,
    DTV_L3_OP_KIND_TRANSFER = 2
} dtv_l3_op_kind;

typedef enum dtv_l3_transfer_kind {
    DTV_L3_TRANSFER_BULK = 1,
    DTV_L3_TRANSFER_CONTROL = 2
} dtv_l3_transfer_kind;

typedef enum dtv_l3_claim_residual_state {
    DTV_L3_CLAIM_NONE = 0,
    DTV_L3_CLAIM_RETAINED = 1,
    DTV_L3_CLAIM_UNKNOWN = 2
} dtv_l3_claim_residual_state;

typedef enum dtv_l3_release_quiescence {
    DTV_L3_RELEASE_QUIESCENCE_UNKNOWN = 0,
    DTV_L3_RELEASE_QUIESCED = 1
} dtv_l3_release_quiescence;

typedef enum dtv_l3_restore_result {
    DTV_L3_RESTORE_NOT_ATTEMPTED = 0,
    DTV_L3_RESTORE_SUCCEEDED = 1,
    DTV_L3_RESTORE_FAILED = 2,
    DTV_L3_RESTORE_UNKNOWN = 3
} dtv_l3_restore_result;

typedef enum dtv_l3_open_state {
    DTV_L3_OPEN_OPENING = 0,
    DTV_L3_OPEN_READY = 1,
    DTV_L3_OPEN_FAILED = 2,
    DTV_L3_OPEN_CLOSING = 3,
    DTV_L3_OPEN_CLOSED = 4
} dtv_l3_open_state;

/* EXITED means the lane function returned; join still happens in context_release. */
typedef enum dtv_l3_lane_state {
    DTV_L3_LANE_NOT_SPAWNED = 0,
    DTV_L3_LANE_RUNNING = 1,
    DTV_L3_LANE_EXIT_COMMITTED = 2,
    DTV_L3_LANE_EXITED = 3
} dtv_l3_lane_state;

typedef struct dtv_l3_transfer_request {
    uint32_t kind;
    uint32_t endpoint;
    uint32_t timeout_ms;
    const uint8_t *out_data;
    size_t out_size;
    size_t in_capacity;
    uint32_t claim_index;
    struct {
        uint8_t request_type;
        uint8_t request;
        uint16_t value;
        uint16_t index;
    } control;
} dtv_l3_transfer_request;

typedef struct dtv_l3_transfer_completion {
    dtv_l3_result result;
    const uint8_t *data;
    size_t size;
} dtv_l3_transfer_completion;

/*
 * A binding is context-local. Request, completion, and cancel identify a
 * transfer with this binding plus its local ID. Keep it alive until accepted
 * transfers have a terminal callback, their storage is recovered, and the
 * owner is released.
 */
typedef void (*dtv_l3_transfer_complete_fn)(
    dtv_l3_provider_binding *binding,
    uint64_t transfer_id,
    const dtv_l3_transfer_completion *completion,
    void *callback_context);

/* ACCEPTED means exactly one terminal callback later; rejection means none. */
typedef dtv_l3_result (*dtv_l3_transfer_submit_fn)(
    void *provider_context,
    dtv_l3_provider_binding *binding,
    uint64_t transfer_id,
    const dtv_l3_transfer_request *request,
    dtv_l3_transfer_complete_fn complete,
    void *callback_context);

typedef dtv_l3_result (*dtv_l3_transfer_cancel_fn)(
    void *provider_context,
    dtv_l3_provider_binding *binding,
    uint64_t transfer_id);

/* Cancellation acceptance is not terminal; a raced normal completion is valid. */

typedef struct dtv_l3_claim_result {
    dtv_l3_result result;
    dtv_l3_claim_residual_state residual_state;
} dtv_l3_claim_result;

/* Quiescence, ownership, restoration, and the reported error are independent. */
typedef struct dtv_l3_release_result {
    dtv_l3_release_quiescence quiescence;
    dtv_l3_claim_residual_state residual_state;
    dtv_l3_restore_result restoration;
    dtv_l3_result error;
} dtv_l3_release_result;

/* OK+RETAINED is transfer-ready; FAILED residuals still belong to the owner. */
typedef dtv_l3_claim_result (*dtv_l3_claim_fn)(
    void *provider_context,
    dtv_l3_provider_binding *binding,
    uint32_t claim_index);

typedef dtv_l3_release_result (*dtv_l3_release_claim_fn)(
    void *provider_context,
    dtv_l3_provider_binding *binding,
    uint32_t claim_index);

typedef enum dtv_l3_lease_compatibility {
    DTV_L3_LEASE_SHAREABLE = 1,
    DTV_L3_LEASE_EXCLUSIVE = 2
} dtv_l3_lease_compatibility;

typedef enum dtv_l3_lease_state {
    DTV_L3_LEASE_FREE = 0,
    DTV_L3_LEASE_TUNED = 1,
    DTV_L3_LEASE_ARMED = 2,
    DTV_L3_LEASE_ACTIVE = 3,
    DTV_L3_LEASE_RETAINED = 4
} dtv_l3_lease_state;

typedef enum dtv_l3_stream_phase {
    DTV_L3_STREAM_ACTIVE = 0,
    DTV_L3_STREAM_DRAINING = 1,
    DTV_L3_STREAM_RETIRING = 2,
    DTV_L3_STREAM_RETIRED = 3
} dtv_l3_stream_phase;

typedef enum dtv_l3_read_state {
    DTV_L3_READ_DATA = 0,
    DTV_L3_READ_WAIT = 1,
    DTV_L3_READ_EOF = 2
} dtv_l3_read_state;

typedef enum dtv_l3_stream_release_mode {
    DTV_L3_STREAM_RELEASE_DRAINED = 1,
    DTV_L3_STREAM_RELEASE_DISCARD_UNREAD = 2
} dtv_l3_stream_release_mode;

/* Prototype scope declaration; bridge and compatibility values are candidates. */
typedef struct dtv_l3_scope_info {
    uint32_t bridge_id;
    uint32_t compatibility;
} dtv_l3_scope_info;

/* data==NULL and size==0 reports the terminal EOF for this stream generation. */
typedef void (*dtv_l3_ts_deliver_fn)(void *callback_context,
                                     const uint8_t *data,
                                     size_t size);

typedef dtv_l3_result (*dtv_l3_scope_query_fn)(void *provider_context,
                                               uint32_t receiver_id,
                                               dtv_l3_scope_info *scope);
typedef dtv_l3_result (*dtv_l3_bridge_start_fn)(
    void *provider_context,
    dtv_l3_provider_binding *binding,
    uint32_t bridge_id,
    uint32_t receiver_id);
typedef dtv_l3_result (*dtv_l3_bridge_stop_fn)(void *provider_context,
                                               dtv_l3_provider_binding *binding,
                                               uint32_t bridge_id);
typedef dtv_l3_result (*dtv_l3_stream_attach_fn)(
    void *provider_context,
    dtv_l3_provider_binding *binding,
    uint64_t stream_id,
    dtv_l3_ts_deliver_fn deliver,
    void *callback_context);
typedef dtv_l3_result (*dtv_l3_stream_stop_fn)(
    void *provider_context,
    dtv_l3_provider_binding *binding,
    uint64_t stream_id);
typedef dtv_l3_result (*dtv_l3_tune_fn)(void *provider_context,
                                        dtv_l3_provider_binding *binding,
                                        uint32_t receiver_id,
                                        uint64_t requested_hz,
                                        uint64_t *programmed_hz);

typedef struct dtv_l3_provider {
    void *provider_context;
    dtv_l3_claim_fn claim;
    dtv_l3_release_claim_fn release_claim;
    dtv_l3_transfer_submit_fn submit;
    dtv_l3_transfer_cancel_fn cancel;
    dtv_l3_scope_query_fn scope_query;
    dtv_l3_bridge_start_fn bridge_start;
    dtv_l3_bridge_stop_fn bridge_stop;
    dtv_l3_stream_attach_fn stream_attach;
    dtv_l3_stream_stop_fn stream_stop;
    dtv_l3_tune_fn tune;
} dtv_l3_provider;

typedef struct dtv_l3_open_result {
    dtv_l3_result result;
    dtv_l3_context *owner;
    dtv_l3_op *op;
} dtv_l3_open_result;

typedef struct dtv_l3_open_status {
    dtv_l3_open_state state;
    dtv_l3_result result;
    uint32_t claims_acquired;
    uint32_t claims_attempted;
    uint32_t cleanup_claims_remaining;
    dtv_l3_result last_cleanup_error;
    uint8_t restoration_failed;
    uint8_t reserved[3];
} dtv_l3_open_status;

typedef struct dtv_l3_close_status {
    dtv_l3_result result;
    uint64_t cleanup_epoch;
    uint8_t physical_quiesced;
    uint8_t all_ops_recovered;
    uint8_t cleanup_complete;
    uint8_t releasable;
    dtv_l3_lane_state open_lane;
    dtv_l3_lane_state control_lane;
    uint32_t live_lanes;
} dtv_l3_close_status;

typedef struct dtv_l3_op_result {
    dtv_l3_op_kind kind;
    dtv_l3_op_state state;
    dtv_l3_result result;
    uint64_t cleanup_epoch;
    const uint8_t *data;
    size_t data_size;
} dtv_l3_op_result;

typedef struct dtv_l3_lease_status {
    dtv_l3_lease_state state;
    uint32_t receiver_id;
    uint32_t bridge_id;
    uint32_t bridge_active_users;
    uint32_t compatibility;
    uint64_t tune_revision;
    uint64_t stream_generation;
    uint64_t requested_hz;
    uint64_t programmed_hz;
    uint8_t bridge_running;
    uint8_t stop_residual;
    uint8_t reserved[2];
} dtv_l3_lease_status;

typedef struct dtv_l3_stream_status {
    dtv_l3_stream_phase phase;
    uint64_t stream_generation;
    uint64_t tune_revision;
    uint64_t queued_packets;
    uint64_t staged_bytes;
    uint64_t dropped_overflow_packets;
    uint64_t discarded_unread_packets;
    uint8_t eof_observed;
    uint8_t read_borrows;
    uint8_t reserved[6];
} dtv_l3_stream_status;

/* ACCEPTED transfers an owner before claim work; later failure keeps it recoverable. */
dtv_l3_open_result dtv_l3_open(const dtv_l3_provider *provider,
                               uint32_t claim_count);
dtv_l3_result dtv_l3_open_status_get(
    dtv_l3_context *owner,
    dtv_l3_open_status *status);

/* A timeout is an observation; it does not release owner or transfer storage.
 * Close never joins a lane. Inside a provider/host callback frame it does not
 * wait and returns OK when already settled or ACCEPTED otherwise. */
dtv_l3_result dtv_l3_close(dtv_l3_context *owner, uint32_t timeout_ms);
dtv_l3_result dtv_l3_close_status_get(
    dtv_l3_context *owner,
    dtv_l3_close_status *status);
dtv_l3_result dtv_l3_cleanup_retry(dtv_l3_context *owner);

/*
 * The host stops issuing calls and drains calls already being issued before
 * final release. Release is not concurrent with another call on this owner.
 * BUSY preserves owner; success retires it once. Use of the pointer afterward
 * is outside this contract.
 */
dtv_l3_result dtv_l3_context_release(dtv_l3_context *owner);

dtv_l3_result dtv_l3_transfer_start(
    dtv_l3_context *owner,
    const dtv_l3_transfer_request *request,
    dtv_l3_op **op);
dtv_l3_result dtv_l3_op_cancel(dtv_l3_op *op);
/* Transfer result data remains valid until this operation handle is released. */
dtv_l3_result dtv_l3_op_poll(dtv_l3_op *op, dtv_l3_op_result *result);
/*
 * Waits only for the operation state; it never runs provider work. OK means the
 * operation reached a terminal state, which op_poll reports. DTV_L3_WAIT_TIMEOUT
 * is only the wait deadline and is not the operation's terminal TIMEOUT.
 * DTV_L3_CLOSING means the owner is closing. BUSY is returned inside a callback
 * frame, where the call observes without waiting.
 */
dtv_l3_result dtv_l3_op_wait(dtv_l3_op *op, uint32_t timeout_ms);
dtv_l3_result dtv_l3_op_release(dtv_l3_op *op);

/*
 * Prototype lease/stream candidates. Receiver, bridge, and generation values
 * are context-local candidates; queue capacity and deadlines are not frozen.
 */
dtv_l3_result dtv_l3_lease_acquire(dtv_l3_context *owner,
                                   uint32_t receiver_id,
                                   dtv_l3_lease **lease_out);
dtv_l3_result dtv_l3_lease_tune(dtv_l3_lease *lease,
                                uint64_t requested_hz,
                                uint64_t *programmed_hz,
                                uint64_t *tune_revision);
dtv_l3_result dtv_l3_lease_start(dtv_l3_lease *lease);
dtv_l3_result dtv_l3_lease_attach(dtv_l3_lease *lease,
                                  dtv_l3_stream **stream_out);
dtv_l3_result dtv_l3_lease_status_get(dtv_l3_lease *lease,
                                      dtv_l3_lease_status *status);

/* OK retires the lease. BUSY keeps the handle for a later retry. */
dtv_l3_result dtv_l3_lease_release(dtv_l3_lease *lease);

dtv_l3_result dtv_l3_stream_stop(dtv_l3_stream *stream, uint32_t timeout_ms);
/* Returns whole 188-byte packets only; capacity_bytes must be 188-aligned. */
dtv_l3_result dtv_l3_stream_read_ts(dtv_l3_stream *stream,
                                    uint8_t *buffer,
                                    size_t capacity_bytes,
                                    size_t *bytes_read,
                                    dtv_l3_read_state *state);
/*
 * Waits for data, EOF, or a terminal stream state without running provider
 * work. DTV_L3_WAIT_TIMEOUT leaves the stream active with state WAIT.
 * DTV_L3_CLOSING is a close observation and does not consume unread data.
 */
dtv_l3_result dtv_l3_stream_read_ts_wait(dtv_l3_stream *stream,
                                         uint8_t *buffer,
                                         size_t capacity_bytes,
                                         size_t *bytes_read,
                                         dtv_l3_read_state *state,
                                         uint32_t timeout_ms);
dtv_l3_result dtv_l3_stream_status_get(dtv_l3_stream *stream,
                                       dtv_l3_stream_status *status);
/* DRAINED is BUSY until EOF is observed; DISCARD_UNREAD retires explicitly. */
dtv_l3_result dtv_l3_stream_release(dtv_l3_stream *stream,
                                    dtv_l3_stream_release_mode mode);
/* Frees the token. A retired token stays readable as STALE until destroyed. */
dtv_l3_result dtv_l3_stream_destroy(dtv_l3_stream *stream);

/* CARD/LNB stay UNSUPPORTED in this increment; no provider side effect. */
dtv_l3_result dtv_l3_reader_acquire(dtv_l3_context *owner,
                                    uint32_t reader_id,
                                    dtv_l3_reader **reader_out);
dtv_l3_result dtv_l3_lnb_set(dtv_l3_context *owner,
                             uint32_t receiver_id,
                             uint32_t mode);

#ifdef __cplusplus
}
#endif

#endif
