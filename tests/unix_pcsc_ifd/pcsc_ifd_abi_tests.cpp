// SPDX-License-Identifier: GPL-2.0-only
extern "C" {
#include <ifdhandler.h>
}

int main()
{
    return IFDHCreateChannel(1U, 0U) == IFD_NO_SUCH_DEVICE ? 0 : 1;
}
