#include <dtv/userland/detail/control_authority.h>

#include <type_traits>

static_assert(std::is_class<dtv::userland::detail::ControlAuthority>::value,
              "authority component header is available");
static_assert(std::is_same<
                  decltype(dtv::userland::detail::AuthorityCompletion{}.kind),
                  dtv::userland::detail::AuthorityJobKind>::value,
              "typed authority completion is available to installed consumers");

int main()
{
    return 0;
}
