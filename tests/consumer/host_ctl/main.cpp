// SPDX-License-Identifier: GPL-2.0-only
#include <host_ctl.h>

int main()
{
    const dtv::userland::host_ctl::Profile profile{
        "sianoctl", "siano-userland", "consumer", dtv::userland::identity::Product::siano};
    const char* arguments[] = {"sianoctl", "--help"};
    return dtv::userland::host_ctl::run_control_cli(2, arguments, profile);
}
