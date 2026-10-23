// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>
#include <host_ctl.h>
#include <px4/posix_ipc.h>
#include <px4_windows_args.h>
#include <px4d_signals.h>
#if defined(_WIN32)
#include <px4/windows_tuner_nonce.h>
#else
#include <px4/posix_tuner_nonce.h>
#endif

#include <type_traits>

static_assert(std::is_same_v<
    px4::userland::ipc::posix::EnclosureFamily,
    dtv::userland::host_runtime::EnclosureFamily>);

int main()
{
    const auto channel = px4::userland::cli::parse_px4_ts_channel("T13");
    if (!channel.valid || channel.system != px4::userland::ipc::System::ISDB_T) return 1;
    using RunWire = px4::userland::Result<px4::userland::cli::Px4TsRunResult> (*)(
        const px4::userland::cli::Px4TsArguments&,
        px4::userland::cli::Px4TsOutput&,
        px4::userland::cli::Px4TsClock&,
        const px4::userland::cli::Px4TsSignal&,
        px4::userland::cli::Px4TsFailureKind*,
        px4::userland::cli::Px4TsRunDiagnostics*) noexcept;
    (void)static_cast<RunWire>(&px4::userland::cli::Px4TsRunner::run_wire);
    const auto profile = dtv::userland::host_ctl::Profile{
        "consumer", "siano-userland", "test", dtv::userland::identity::Product::siano};
    const char* ctl_arguments[] = {"consumer", "--help"};
    if (dtv::userland::host_ctl::run_control_cli(2, ctl_arguments, profile) != 0 ||
        !px4::userland::px4d::install_signal_handlers()) return 1;
    px4::userland::px4d::request_stop();
    if (!px4::userland::px4d::stop_requested()) return 1;
#if defined(_WIN32)
    px4::userland::ipc::windows::WindowsTunerNonceSource nonce_source;
    if (!px4::userland::cli::windows_argv_utf8(0, nullptr).size()) return 1;
#else
    px4::userland::ipc::posix::PosixTunerNonceSource nonce_source;
#endif
    return nonce_source.generate() ? 0 : 1;
}
