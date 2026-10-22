#include <px4/host_authority_owner.h>

int main()
{
    return sizeof(px4::userland::HostAuthorityOwner) > 0U ? 0 : 1;
}
