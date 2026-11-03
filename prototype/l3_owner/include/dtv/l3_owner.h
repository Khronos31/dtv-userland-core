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

typedef enum dtv_l3_result {
    DTV_L3_OK = 0,
    DTV_L3_ACCEPTED = 1,
    DTV_L3_BUSY = 2,
    DTV_L3_TIMEOUT = 3,
    DTV_L3_CANCELLED = 4,
    DTV_L3_UNSUPPORTED = 5,
    DTV_L3_INVALID = 6,
    DTV_L3_FAILED = 7
} dtv_l3_result;

typedef enum dtv_l3_op_state {
    DTV_L3_OP_OPENING = 0,
    DTV_L3_OP_RUNNING = 1,
    DTV_L3_OP_DONE = 2,
    DTV_L3_OP_FAILED = 3
} dtv_l3_op_state;

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

typedef struct dtv_l3_provider {
    void *provider_context;
    dtv_l3_claim_fn claim;
    dtv_l3_release_claim_fn release_claim;
    dtv_l3_transfer_submit_fn submit;
    dtv_l3_transfer_cancel_fn cancel;
} dtv_l3_provider;

typedef struct dtv_l3_open_result {
    dtv_l3_result result;
    dtv_l3_context *owner;
} dtv_l3_open_result;

typedef struct dtv_l3_open_status {
    dtv_l3_open_state state;
    uint32_t claims_acquired;
    uint32_t claims_attempted;
    uint32_t cleanup_claims_remaining;
    dtv_l3_result last_cleanup_error;
    uint8_t restoration_failed;
    uint8_t reserved[3];
} dtv_l3_open_status;

typedef struct dtv_l3_close_status {
    dtv_l3_result result;
    uint8_t physical_quiesced;
    uint8_t all_ops_recovered;
    uint8_t cleanup_complete;
    uint8_t reserved;
} dtv_l3_close_status;

/* ACCEPTED transfers an owner before claim work; later failure keeps it recoverable. */
dtv_l3_open_result dtv_l3_open(const dtv_l3_provider *provider,
                               uint32_t claim_count);
dtv_l3_result dtv_l3_open_status_get(
    dtv_l3_context *owner,
    dtv_l3_open_status *status);

/* A timeout is an observation; it does not release owner or transfer storage. */
dtv_l3_result dtv_l3_close(dtv_l3_context *owner, uint32_t timeout_ms);
dtv_l3_result dtv_l3_close_status_get(
    dtv_l3_context *owner,
    dtv_l3_close_status *status);

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
dtv_l3_result dtv_l3_op_poll(dtv_l3_op *op, dtv_l3_op_state *state);
dtv_l3_result dtv_l3_op_release(dtv_l3_op *op);

#ifdef __cplusplus
}
#endif

#endif
