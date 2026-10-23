// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/userland/host_facade/px4d_signals.h>
#include <dtv/userland/host_runtime/family.h>
#if defined(_WIN32)
#include <px4/windows_tuner_nonce.h>
#else
#include <px4/posix_tuner_nonce.h>
#endif

int main()
{
#if defined(_WIN32)
    px4::userland::ipc::windows::WindowsTunerNonceSource source;
#else
    px4::userland::ipc::posix::PosixTunerNonceSource source;
#endif
    const auto nonce = source.generate();
    if (!nonce || !px4::userland::px4d::install_signal_handlers()) return 1;
    px4::userland::px4d::request_stop();
    return px4::userland::px4d::stop_requested() ? 0 : 1;
}
