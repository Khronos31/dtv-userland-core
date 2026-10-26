// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_HOST_TS_PX4_TS_CORE_H
#define DTV_HOST_TS_PX4_TS_CORE_H

#include "px4/error.h"
#include "px4/ipc.h"
#include "px4/posix_ipc.h"
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/identity.h>
#include <dtv/userland/records.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>

namespace px4::userland::cli {

enum class Px4TsFailureKind : std::uint8_t {
    ipc,
    ts_integrity,
    output,
};

// How a channel name determines the ISDB-S slot.  satellite_transponder
// carries no slot and needs --slot or --stream-id; satellite_slot fixes it in
// the notation; satellite_cs defaults to slot 0 but accepts an override.
enum class Px4TsChannelKind : std::uint8_t {
    terrestrial,
    satellite_transponder,
    satellite_slot,
    satellite_cs,
};

struct Px4TsChannel final {
    bool valid = false;
    ipc::System system = ipc::System::ISDB_T;
    std::uint64_t frequency_khz = 0U;
    Px4TsChannelKind kind = Px4TsChannelKind::terrestrial;
    std::uint16_t slot = 0xffffU;
};

// Pure conversion from a channel name (T27, 27, BS01_0, BS01, CS2) to the
// tune parameters.  I/O-free so the CLI parser and unit tests share one
// implementation.
Px4TsChannel parse_px4_ts_channel(std::string_view text) noexcept;

struct Px4TsArguments final {
    bool valid = false;
    bool help = false;
    bool version = false;
    bool product_cli = false;
    bool common_profile = false;
    bool receiver_explicit = false;
    bool group = false;
    std::string device;
    std::string instance;
    std::string runtime_directory;
    std::string output;
    std::uint16_t receiver = 0U;
    ipc::System system = ipc::System::ISDB_T;
    std::uint64_t frequency_khz = 0U;
    // Exact canonical Hz entrance used by products that resolve the frequency
    // in the shared tuning authority.  Exactly one of frequency_khz and
    // frequency_hz is set.
    std::uint64_t frequency_hz = 0U;
    std::uint16_t stream_id = 0xffffU;
    std::uint16_t slot = 0xffffU;
    std::uint32_t bandwidth_hz = 6000000U;
    std::uint8_t lnb_voltage = 0U;
    std::uint32_t tune_timeout_ms = 10000U;
    std::uint64_t duration_seconds = 0U;
    std::uint64_t packet_count = 0U;
    bool duration_set = false;
    bool packet_count_set = false;
    std::string error;
};

struct TsClientProfile final {
    const char* tool_name = nullptr;
    const char* product_slug = nullptr;
    dtv::userland::identity::Product product = dtv::userland::identity::Product::px4;
    bool serial_selector_supported = false;
    bool preserve_instance_case = false;
    // Product-specific validation after common serial checks; PX4 requires it, PIX uses common checks.
    bool (*matches_serial_identity)(
        const dtv::userland::wire::ListResponse&,
        const dtv::userland::identity::ResolvedIdentity&) noexcept = nullptr;
    bool frequency_system_optional = false;
};

Px4TsArguments parse_px4_ts_arguments(int argc,
                                      const char* const* argv) noexcept;
Px4TsArguments parse_px4_ts_product_arguments(int argc,
                                              const char* const* argv) noexcept;
Px4TsArguments parse_px4_ts_product_arguments(
    int argc, const char* const* argv, const TsClientProfile& profile) noexcept;
void print_px4_ts_usage(void* output) noexcept;
void print_px4_ts_usage(const TsClientProfile& profile, void* output) noexcept;
int px4_ts_exit_status(Error error,
                       Px4TsFailureKind kind = Px4TsFailureKind::ipc) noexcept;

class Px4TsSignal;

class Px4TsOutput {
public:
    virtual ~Px4TsOutput() noexcept = default;
    virtual Result<void> write(ByteView bytes, const Px4TsSignal& signal) noexcept = 0;
};

class Px4TsClock {
public:
    virtual ~Px4TsClock() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

class Px4TsSignal {
public:
    virtual ~Px4TsSignal() noexcept = default;
    virtual bool stop_requested() const noexcept = 0;
};

struct Px4TsRunResult final {
    ipc::CountersPayload counters{};
    bool signal_stopped = false;
};

enum class Px4TsDiagnosticStage : std::uint8_t {
    none,
    endpoint_connect,
    receiver_selection,
    recording_setup,
};

// Best-effort diagnostics for a failed CLI run.  These values are never used
// for protocol decisions; they only make a daemon/consumer invariant failure
// actionable without contaminating stdout with non-TS bytes.
struct Px4TsRunDiagnostics final {
    Px4TsDiagnosticStage stage = Px4TsDiagnosticStage::none;
    dtv::userland::host_runtime::EndpointConnectDiagnostic endpoint_connect{};
    std::uint16_t receiver_count = 0U;
    std::uint16_t requested_receiver = 0U;
    bool receiver_explicit = false;
    ipc::System requested_system = ipc::System::ISDB_T;
    bool stop_counters_valid = false;
    ipc::CountersPayload stop_counters{};
    bool stream_end_valid = false;
    ipc::StreamEndEventPayload stream_end{};
    std::uint64_t packets_written = 0U;
};

void print_px4_ts_run_diagnostic(
    std::FILE* output, const char* tool_name, Error error,
    const Px4TsRunDiagnostics& diagnostics) noexcept;

// Private CLI core. POSIX I/O adapters are supplied by px4-ts.cpp; this class
// owns the control/data protocol and cleanup ordering and never writes a
// socket or output stream from a signal handler.
class Px4TsRunner final {
public:
    // Normal UDTV path: HELLO/LIST/STATUS/ACQUIRE/TUNE/START, typed ATTACH,
    // typed TS_DATA/STREAM_END, then STOP and RELEASE.  -t/--time starts when
    // the AttachResponse is accepted.
    static Result<Px4TsRunResult> run_wire(
        const Px4TsArguments& arguments, Px4TsOutput& output,
        Px4TsClock& clock, const Px4TsSignal& signal,
        Px4TsFailureKind* failure_kind = nullptr,
        Px4TsRunDiagnostics* diagnostics = nullptr) noexcept;
    static Result<Px4TsRunResult> run_wire(
        const Px4TsArguments& arguments, const TsClientProfile& profile,
        Px4TsOutput& output, Px4TsClock& clock, const Px4TsSignal& signal,
        Px4TsFailureKind* failure_kind = nullptr,
        Px4TsRunDiagnostics* diagnostics = nullptr) noexcept;
};

}  // namespace px4::userland::cli

#endif  // DTV_HOST_TS_PX4_TS_CORE_H
