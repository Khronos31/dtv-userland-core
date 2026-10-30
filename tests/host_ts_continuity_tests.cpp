// Modified/ported for dtv-userland-core on 2026-10-30.
//
// Derived from px4-userland userland/tests/ts_continuity_tests.cpp at
// 5c91e8c57b288c0f490061789829e42dbbf61b3d. Case names and expected
// classifications are preserved; only the Core namespace differs.
// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_ts_continuity/ts_continuity.h"

#include <array>
#include <cstdio>
#include <memory>
#include <new>

namespace {

using dtv::userland::host_ts_continuity::TsContinuityChecker;
using dtv::userland::host_ts_continuity::TsContinuityKind;
using Packet = std::array<std::uint8_t, TsContinuityChecker::kPacketSize>;
using Kind = TsContinuityKind;

#define CC_CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::fprintf(stderr, "CC check failed at %s:%d: %s\n", \
                         __FILE__, __LINE__, #expression); \
            return false; \
        } \
    } while (false)

Packet packet(std::uint16_t pid, std::uint8_t cc,
              std::uint8_t afc = 1U, bool discontinuity = false) noexcept
{
    Packet result{};
    result.fill(0xffU);
    result[0U] = 0x47U;
    result[1U] = static_cast<std::uint8_t>((pid >> 8U) & 0x1fU);
    result[2U] = static_cast<std::uint8_t>(pid);
    result[3U] = static_cast<std::uint8_t>((afc << 4U) | (cc & 0x0fU));
    if ((afc & 2U) != 0U) {
        result[4U] = afc == 2U ? 183U : 1U;
        result[5U] = discontinuity ? 0x80U : 0U;
    }
    return result;
}

Kind observe(TsContinuityChecker& checker, const Packet& value) noexcept
{
    return checker.observe(value.data(), value.size()).kind;
}

bool test_counter_progression(TsContinuityChecker& checker)
{
    // Every wrap/initial value, and rebaseline after a real error.
    for (unsigned start = 0U; start < 16U; ++start) {
        checker.reset();
        CC_CHECK(observe(checker, packet(0x100U, start)) == Kind::initial);
        for (unsigned step = 1U; step <= 32U; ++step)
            CC_CHECK(observe(checker, packet(0x100U, start + step)) == Kind::continuous);
        CC_CHECK(observe(checker, packet(0x100U, start + 34U)) == Kind::error);
        CC_CHECK(observe(checker, packet(0x100U, start + 35U)) == Kind::continuous);
    }
    checker.reset();
    CC_CHECK(observe(checker, packet(0x100U, 5U)) == Kind::initial);
    CC_CHECK(observe(checker, packet(0x100U, 5U, 2U)) == Kind::continuous);
    CC_CHECK(observe(checker, packet(0x100U, 5U, 2U)) == Kind::continuous);
    CC_CHECK(observe(checker, packet(0x100U, 6U)) == Kind::continuous);
    CC_CHECK(observe(checker, packet(0x100U, 9U, 2U)) == Kind::error);
    CC_CHECK(observe(checker, packet(0x100U, 10U)) == Kind::continuous);
    checker.reset();
    CC_CHECK(observe(checker, packet(0x100U, 9U, 2U)) == Kind::initial);
    CC_CHECK(observe(checker, packet(0x100U, 10U)) == Kind::continuous);
    return true;
}

bool test_duplicate_pairs(TsContinuityChecker& checker)
{
    checker.reset();
    const Packet first = packet(0x100U, 5U);
    CC_CHECK(observe(checker, first) == Kind::initial);
    // Consecutive means consecutive within this PID, not adjacent in the mux.
    CC_CHECK(observe(checker, packet(0x101U, 12U)) == Kind::initial);
    CC_CHECK(observe(checker, first) == Kind::duplicate);
    for (unsigned repeat = 0U; repeat < 6U; ++repeat)
        CC_CHECK(observe(checker, first) == Kind::error);
    CC_CHECK(observe(checker, packet(0x100U, 6U)) == Kind::continuous);
    CC_CHECK(observe(checker, packet(0x100U, 6U)) == Kind::duplicate);

    checker.reset();
    CC_CHECK(observe(checker, first) == Kind::initial);
    Packet conflicting = first;
    conflicting.back() ^= 1U;
    CC_CHECK(observe(checker, conflicting) == Kind::error);
    CC_CHECK(observe(checker, conflicting) == Kind::error);
    CC_CHECK(observe(checker, packet(0x100U, 6U)) == Kind::continuous);

    checker.reset();
    CC_CHECK(observe(checker, first) == Kind::initial);
    CC_CHECK(observe(checker, packet(0x100U, 5U, 2U)) == Kind::continuous);
    CC_CHECK(observe(checker, first) == Kind::error);
    CC_CHECK(observe(checker, packet(0x100U, 6U)) == Kind::continuous);

    checker.reset();
    CC_CHECK(observe(checker, first) == Kind::initial);
    CC_CHECK(observe(checker, packet(0x100U, 6U)) == Kind::continuous);
    CC_CHECK(observe(checker, first) == Kind::error);  // delayed retransmission
    return true;
}

Packet pcr_packet() noexcept
{
    Packet value = packet(0x100U, 5U, 3U);
    value[4U] = 13U;  // PCR plus OPCR, both retained in packet comparison
    value[5U] = 0x18U;
    for (std::size_t index = 6U; index < 18U; ++index) value[index] = 0U;
    value[10U] = 0x7eU;
    value[16U] = 0x7eU;
    return value;
}

bool test_pcr_duplicate_exception(TsContinuityChecker& checker)
{
    const Packet first = pcr_packet();
    Packet later = first;
    later[6U] = 0x81U;
    later[7U] = 0x34U;
    later[8U] = 0x56U;
    later[9U] = 0x78U;
    later[10U] = 0xffU;  // last base bit, reserved ones, extension high bit
    later[11U] = 43U;    // maximum valid extension: 299
    checker.reset();
    CC_CHECK(observe(checker, first) == Kind::initial);
    CC_CHECK(observe(checker, later) == Kind::duplicate);
    later[6U] ^= 1U;
    CC_CHECK(observe(checker, later) == Kind::error);

    // Changed PUSI/scrambling, adaptation flags/length, OPCR, stuffing, and
    // payload are conflicts. None receives the PCR-only comparison exception.
    constexpr std::array<std::size_t, 10U> positions{
        1U, 3U, 4U, 5U, 12U, 16U, 17U, 18U, 80U, 187U};
    for (std::size_t position : positions) {
        checker.reset();
        CC_CHECK(observe(checker, first) == Kind::initial);
        Packet changed = later;
        changed[position] ^= position == 1U ? 0x40U : position == 3U ? 0x80U : 1U;
        CC_CHECK(observe(checker, changed) == Kind::error);
    }
    checker.reset();
    CC_CHECK(observe(checker, first) == Kind::initial);
    Packet reserved = later;
    reserved[10U] &= 0xfdU;
    CC_CHECK(observe(checker, reserved) == Kind::invalid_packet);
    Packet extension = later;
    extension[11U] = 44U;  // 300 is outside the PCR modulus
    CC_CHECK(observe(checker, extension) == Kind::invalid_packet);
    // The trusted CC is retained, but duplicate adjacency is unknowable.
    CC_CHECK(observe(checker, later) == Kind::error);
    return true;
}

bool test_discontinuities(TsContinuityChecker& checker)
{
    for (std::uint8_t afc : {2U, 3U}) {
        checker.reset();
        CC_CHECK(observe(checker, packet(0x100U, 0U)) == Kind::initial);
        CC_CHECK(observe(checker, packet(0x100U, 9U, afc, true)) == Kind::discontinuity);
        CC_CHECK(observe(checker, packet(0x100U, 10U)) == Kind::continuous);
        CC_CHECK(observe(checker, packet(0x100U, 10U, afc, true)) == Kind::discontinuity);
        CC_CHECK(observe(checker, packet(0x100U, 2U)) == Kind::error);
        CC_CHECK(observe(checker, packet(0x100U, 3U)) == Kind::continuous);
    }
    checker.reset();
    CC_CHECK(observe(checker, packet(0x100U, 9U, 2U, true)) == Kind::discontinuity);
    CC_CHECK(observe(checker, packet(0x100U, 9U, 2U)) == Kind::continuous);
    CC_CHECK(observe(checker, packet(0x100U, 10U)) == Kind::continuous);
    // Explicit bounded scope: DI-run/PCR_PID validity requires program
    // context and is not certified by this CC-only component.
    for (std::uint8_t cc : {4U, 8U, 2U})
        CC_CHECK(observe(checker, packet(0x100U, cc, 2U, true)) == Kind::discontinuity);
    return true;
}

bool test_untrusted_headers(TsContinuityChecker& checker)
{
    checker.reset();
    const Packet first = packet(0x100U, 0U);
    CC_CHECK(observe(checker, first) == Kind::initial);
    CC_CHECK(checker.observe(nullptr, 188U).kind == Kind::invalid_packet);
    // These are real short buffers, including no readable header; sanitizer
    // runs verify size checks precede every field access.
    for (std::size_t size = 0U; size < 188U; ++size) {
        std::unique_ptr<std::uint8_t[]> short_packet(new (std::nothrow) std::uint8_t[size]);
        CC_CHECK(short_packet);
        CC_CHECK(checker.observe(short_packet.get(), size).kind == Kind::invalid_packet);
    }
    CC_CHECK(checker.observe(first.data(), 189U).kind == Kind::invalid_packet);
    Packet bad = first;
    bad[0U] = 0U;
    CC_CHECK(observe(checker, bad) == Kind::invalid_packet);
    CC_CHECK(observe(checker, packet(0x100U, 7U, 0U)) == Kind::invalid_packet);
    for (std::uint8_t length : {0U, 1U, 182U, 184U, 255U}) {
        bad = packet(0x100U, 7U, 2U, true);
        bad[4U] = length;
        CC_CHECK(observe(checker, bad) == Kind::invalid_packet);
    }
    for (std::uint8_t length : {183U, 184U, 255U}) {
        bad = packet(0x100U, 7U, 3U, true);
        bad[4U] = length;
        CC_CHECK(observe(checker, bad) == Kind::invalid_packet);
    }
    for (std::uint8_t length : {1U, 2U, 3U, 4U, 5U, 6U}) {
        bad = pcr_packet();
        bad[4U] = length;
        CC_CHECK(observe(checker, bad) == Kind::invalid_packet);
    }
    bad = packet(0x100U, 7U);
    bad[1U] |= 0x80U;
    CC_CHECK(observe(checker, bad) == Kind::transport_error);
    CC_CHECK(observe(checker, packet(0x100U, 1U)) == Kind::continuous);
    bad = packet(0x100U, 2U);
    bad[1U] |= 0x80U;
    CC_CHECK(observe(checker, bad) == Kind::transport_error);
    // Only a later trusted packet establishes a real comparison after TEI.
    CC_CHECK(observe(checker, packet(0x100U, 3U)) == Kind::error);
    CC_CHECK(observe(checker, packet(0x100U, 4U)) == Kind::continuous);

    checker.reset();
    CC_CHECK(observe(checker, first) == Kind::initial);
    bad = packet(0x102U, 7U);
    bad[1U] |= 0x80U;
    CC_CHECK(observe(checker, bad) == Kind::transport_error);
    // A TEI packet's PID is untrusted too: it could have belonged to 0x100.
    CC_CHECK(observe(checker, first) == Kind::error);
    CC_CHECK(observe(checker, packet(0x100U, 1U)) == Kind::continuous);

    checker.reset();
    CC_CHECK(observe(checker, first) == Kind::initial);
    checker.observe_untrusted_packet();
    CC_CHECK(observe(checker, first) == Kind::error);
    CC_CHECK(observe(checker, packet(0x100U, 1U)) == Kind::continuous);

    checker.reset();
    bad = packet(0x100U, 4U, 3U);
    bad[4U] = 0U;  // no flags byte; payload begins at byte 5
    bad[5U] = 0x80U;
    CC_CHECK(observe(checker, bad) == Kind::initial);
    CC_CHECK(observe(checker, packet(0x100U, 5U)) == Kind::continuous);
    bad = packet(0x100U, 6U, 3U);
    bad[4U] = 182U;
    CC_CHECK(observe(checker, bad) == Kind::continuous);
    return true;
}

bool test_pid_and_epoch_isolation(TsContinuityChecker& checker)
{
    checker.reset();
    for (std::uint16_t pid : {0U, 1U, 0x100U, 0x1ffeU}) {
        const Packet value = packet(pid, 12U);
        const auto result = checker.observe(value.data(), value.size());
        CC_CHECK(result.kind == Kind::initial && result.pid == pid &&
                 result.counter == 12U && result.has_payload && !result.discontinuity);
    }
    for (std::uint8_t cc : {0U, 0U, 9U, 3U})
        CC_CHECK(observe(checker, packet(0x1fffU, cc)) == Kind::null_packet);
    for (std::uint16_t pid : {0U, 1U, 0x100U, 0x1ffeU})
        CC_CHECK(observe(checker, packet(pid, 13U)) == Kind::continuous);
    CC_CHECK(observe(checker, packet(0x100U, 13U)) == Kind::duplicate);
    checker.reset();
    CC_CHECK(observe(checker, packet(0x100U, 13U)) == Kind::initial);
    CC_CHECK(observe(checker, packet(0x100U, 13U)) == Kind::duplicate);
    CC_CHECK(observe(checker, packet(0x100U, 13U)) == Kind::error);
    return true;
}

}  // namespace

int main()
{
    // A full checker exceeds the default Windows stack; callers own its
    // lifetime and allocate it once before processing packets.
    std::unique_ptr<TsContinuityChecker> checker(new (std::nothrow) TsContinuityChecker);
    if (!checker || !test_counter_progression(*checker) ||
        !test_duplicate_pairs(*checker) || !test_pcr_duplicate_exception(*checker) ||
        !test_discontinuities(*checker) || !test_untrusted_headers(*checker) ||
        !test_pid_and_epoch_isolation(*checker))
        return 1;
    std::printf("TS continuity regression fixtures passed (checker storage: %zu bytes)\n",
                sizeof(TsContinuityChecker));
    return 0;
}
