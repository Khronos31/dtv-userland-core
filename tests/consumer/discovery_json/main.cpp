// SPDX-License-Identifier: MIT
#include <dtv/userland/discovery_json.h>

int main()
{
    dtv::userland::wire::DiscoveryScan scan;
    scan.complete = true;
    return dtv::userland::wire::discovery_json_size(scan) ? 0 : 1;
}
