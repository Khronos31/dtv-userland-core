// SPDX-License-Identifier: GPL-2.0-only
#include "host_ctl.h"

int main()
{
    const dtv::userland::host_ctl::Profile profile{
        "sianoctl", "siano-userland", "test", dtv::userland::identity::Product::siano};
    const char* help[] = {"sianoctl", "--help"};
    if (dtv::userland::host_ctl::run_control_cli(2, help, profile) != 0) return 1;
    const char* version[] = {"sianoctl", "--version"};
    if (dtv::userland::host_ctl::run_control_cli(2, version, profile) != 0) return 1;
    const char* unsupported[] = {"sianoctl", "card-status"};
    return dtv::userland::host_ctl::run_control_cli(2, unsupported, profile) == 2 ? 0 : 1;
}
