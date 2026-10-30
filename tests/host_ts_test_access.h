// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_HOST_TS_TEST_ACCESS_H
#define DTV_HOST_TS_TEST_ACCESS_H

#include <dtv/host_ts/px4_ts_core.h>
#include "dtv/userland/host_runtime/posix_ipc.h"

namespace px4::userland::cli::test_access {

void fail_next_frame_allocation() noexcept;

dtv::userland::Result<Px4TsRunResult> run_wire_with_roots(
    const Px4TsArguments& arguments, const TsClientProfile& profile,
    Px4TsOutput& output, Px4TsClock& clock, const Px4TsSignal& signal,
    const ipc::posix::RuntimeRootCandidates& roots,
    Px4TsFailureKind* failure_kind,
    Px4TsRunDiagnostics* diagnostics) noexcept;

}  // namespace px4::userland::cli::test_access

#endif  // DTV_HOST_TS_TEST_ACCESS_H
