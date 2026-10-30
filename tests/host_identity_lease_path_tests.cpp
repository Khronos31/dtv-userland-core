// SPDX-License-Identifier: GPL-2.0-only
#include "detail/windows_path.h"

#include <cstdio>
#include <string_view>

using dtv::userland::host_listener_context::detail::windows_path::is_local_absolute;

namespace {

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::fprintf(stderr, "windows path check failed at line %d: %s\n", \
                         __LINE__, #expression); \
            return false; \
        } \
    } while (false)

bool test_accepted_paths()
{
    CHECK(is_local_absolute(L"C:\\"));
    CHECK(is_local_absolute(L"C:\\Users\\test\\runtime"));
    CHECK(is_local_absolute(L"c:\\runtime"));
    CHECK(is_local_absolute(L"\\\\?\\C:\\runtime"));
    CHECK(is_local_absolute(L"\\\\?\\c:\\"));
    return true;
}

bool test_rejected_paths()
{
    CHECK(!is_local_absolute(L""));
    CHECK(!is_local_absolute(L"runtime"));
    CHECK(!is_local_absolute(L".\\runtime"));
    CHECK(!is_local_absolute(L"C:runtime"));
    CHECK(!is_local_absolute(L"C:"));
    CHECK(!is_local_absolute(L"\\runtime"));
    CHECK(!is_local_absolute(L"\\\\server\\share"));
    CHECK(!is_local_absolute(L"\\\\?\\UNC\\server\\share"));
    CHECK(!is_local_absolute(L"\\\\?\\C:"));
    CHECK(!is_local_absolute(L"\\\\?\\C:runtime"));
    CHECK(!is_local_absolute(L"\\\\.\\C:\\runtime"));
    CHECK(!is_local_absolute(L"\\\\?\\C:/runtime"));
    CHECK(!is_local_absolute(L"1:\\runtime"));
    return true;
}

}  // namespace

int main()
{
    if (!test_accepted_paths() || !test_rejected_paths()) return 1;
    std::printf("windows identity lease path fixtures passed\n");
    return 0;
}
