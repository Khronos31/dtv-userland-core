/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/l3.h>

int main(void)
{
    dtv_l3_public_context *context = 0;
    dtv_l3_public_open_args args = {0};
    args.struct_size = (uint32_t)sizeof(args);
    args.location = "1-2.4";

    if (dtv_l3_public_open(0, &context) != DTV_L3_PUBLIC_INVALID) {
        return 1;
    }
    if (dtv_l3_public_open(&args, &context) != DTV_L3_PUBLIC_INVALID) {
        return 2;
    }
    return context == 0 ? 0 : 3;
}
