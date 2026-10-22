#include <dtv/userland/detail/control_authority.h>

#include <type_traits>

static_assert(std::is_class<dtv::userland::detail::ControlAuthority>::value,
              "authority component header is available");

int main()
{
    return 0;
}
