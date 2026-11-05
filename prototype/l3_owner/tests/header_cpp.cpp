/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/l3_owner.h>

int main() {
    dtv_l3_open_result result{DTV_L3_ACCEPTED, nullptr};
    return result.result == DTV_L3_ACCEPTED ? 0 : 1;
}
