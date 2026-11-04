/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_L3_H
#define DTV_L3_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtv_l3_public_context dtv_l3_public_context;
typedef struct dtv_l3_product dtv_l3_product;

typedef enum dtv_l3_public_result {
    DTV_L3_PUBLIC_OK = 0,
    DTV_L3_PUBLIC_ACCEPTED = 1,
    DTV_L3_PUBLIC_TIMEOUT = 2,
    DTV_L3_PUBLIC_UNSUPPORTED = 3,
    DTV_L3_PUBLIC_INVALID = 4,
    DTV_L3_PUBLIC_FAILED = 5,
    DTV_L3_PUBLIC_BUSY = 6
} dtv_l3_public_result;

typedef enum dtv_l3_public_log_level {
    DTV_L3_PUBLIC_LOG_ERROR = 0,
    DTV_L3_PUBLIC_LOG_WARNING = 1,
    DTV_L3_PUBLIC_LOG_INFO = 2,
    DTV_L3_PUBLIC_LOG_DEBUG = 3
} dtv_l3_public_log_level;

typedef void (*dtv_l3_public_log_fn)(void *userdata,
                                     dtv_l3_public_log_level level,
                                     const char *function,
                                     const char *message);

typedef struct dtv_l3_public_logger {
    dtv_l3_public_log_fn fn;
    void *userdata;
    dtv_l3_public_log_level min_level;
} dtv_l3_public_logger;

typedef struct dtv_l3_public_firmware {
    const uint8_t *bytes;
    size_t size;
    const uint8_t *sha256;
    uint32_t format;
} dtv_l3_public_firmware;

typedef struct dtv_l3_public_open_args {
    uint32_t struct_size;
    const dtv_l3_product *product;
    const char *location;
    dtv_l3_public_firmware firmware;
    dtv_l3_public_logger logger;
    uint32_t power_permission;
} dtv_l3_public_open_args;

/* The product library returns its opaque descriptor; firmware and logger data
 * must remain valid until open returns, and logger userdata until close ends. */
/* On ACCEPTED, call wait_ready; call close even when wait_ready reports failure. */
dtv_l3_public_result dtv_l3_public_open(
    const dtv_l3_public_open_args *args,
    dtv_l3_public_context **context_out);
dtv_l3_public_result dtv_l3_public_wait_ready(
    dtv_l3_public_context *context,
    uint32_t timeout_ms);
/* A timed out close keeps the context live so the caller can retry. */
dtv_l3_public_result dtv_l3_public_close(
    dtv_l3_public_context *context,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
