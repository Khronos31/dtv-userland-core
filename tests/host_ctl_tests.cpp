// SPDX-License-Identifier: GPL-2.0-only
#include "host_ctl.h"

#include <array>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

int duplicate_stdout()
{
#if defined(_WIN32)
    return ::_dup(::_fileno(stdout));
#else
    return ::dup(::fileno(stdout));
#endif
}

bool redirect_stdout(FILE* output)
{
#if defined(_WIN32)
    return ::_dup2(::_fileno(output), ::_fileno(stdout)) == 0;
#else
    return ::dup2(::fileno(output), ::fileno(stdout)) >= 0;
#endif
}

void restore_stdout(int saved)
{
#if defined(_WIN32)
    (void)::_dup2(saved, ::_fileno(stdout));
    (void)::_close(saved);
#else
    (void)::dup2(saved, ::fileno(stdout));
    (void)::close(saved);
#endif
}

bool help_text_matches_contract(const dtv::userland::host_ctl::Profile& profile)
{
    FILE* output = std::tmpfile();
    if (output == nullptr) return false;
    std::fflush(stdout);
    const int saved = duplicate_stdout();
    if (saved < 0 || !redirect_stdout(output)) {
        if (saved >= 0) restore_stdout(saved);
        std::fclose(output);
        return false;
    }
    const char* arguments[] = {"sianoctl", "--help"};
    const int status = dtv::userland::host_ctl::run_control_cli(2, arguments, profile);
    std::fflush(stdout);
    restore_stdout(saved);
    std::rewind(output);
    std::string text;
    std::array<char, 256U> buffer{};
    for (;;) {
        const std::size_t count = std::fread(buffer.data(), 1U, buffer.size(), output);
        text.append(buffer.data(), count);
        if (count != buffer.size()) break;
    }
    std::fclose(output);
    return status == 0 && text.find(
        "--runtime-dir overrides the standard runtime root; --instance is required unless --device is supplied") !=
        std::string::npos;
}

bool runtime_dir_is_optional(const dtv::userland::host_ctl::Profile& profile)
{
    const char* arguments[] = {
        "sianoctl", "--instance", "core-default-root-not-running", "status"};
    return dtv::userland::host_ctl::run_control_cli(4, arguments, profile) != 2;
}

}  // namespace

int main()
{
    const dtv::userland::host_ctl::Profile profile{
        "sianoctl", "siano-userland", "test", dtv::userland::identity::Product::siano};
    if (!help_text_matches_contract(profile) || !runtime_dir_is_optional(profile)) return 1;
    const char* version[] = {"sianoctl", "--version"};
    if (dtv::userland::host_ctl::run_control_cli(2, version, profile) != 0) return 1;
    const char* unsupported[] = {"sianoctl", "card-status"};
    if (dtv::userland::host_ctl::run_control_cli(2, unsupported, profile) != 3) return 1;
    const char* unsupported_atr[] = {"sianoctl", "card-atr"};
    if (dtv::userland::host_ctl::run_control_cli(2, unsupported_atr, profile) != 3) return 1;
    const char* unsupported_apdu[] = {"sianoctl", "card-apdu", "00:01"};
    if (dtv::userland::host_ctl::run_control_cli(3, unsupported_apdu, profile) != 3)
        return 1;
    const char* invalid_apdu[] = {"sianoctl", "card-apdu", "0:01"};
    if (dtv::userland::host_ctl::run_control_cli(3, invalid_apdu, profile) != 2)
        return 1;
    const char* repeat_apdu[] = {"sianoctl", "card-apdu", "00:01", "--repeat", "2"};
    return dtv::userland::host_ctl::run_control_cli(5, repeat_apdu, profile) == 2 ? 0 : 1;
}
