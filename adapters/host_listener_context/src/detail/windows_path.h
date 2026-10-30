// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_WINDOWS_PATH_H
#define DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_WINDOWS_PATH_H

#include <string_view>

namespace dtv::userland::host_listener_context::detail::windows_path {

// Same-host IPC may use only a local absolute runtime path. Accept
// drive-absolute ("C:\...") and extended drive-absolute ("\\?\C:\...");
// reject relative, drive-relative ("C:..."), rooted-relative ("\..."),
// UNC/remote ("\\server\share", "\\?\UNC\...") and incomplete extended
// forms. The ASICEN reference contract rejected UNC; its prefix-only
// extended branch did not check the drive separator, so this predicate
// enforces the stated contract.
inline bool is_local_absolute(std::wstring_view path) noexcept
{
    const auto letter = [](wchar_t character) noexcept {
        return (character >= L'A' && character <= L'Z') ||
               (character >= L'a' && character <= L'z');
    };
    if (path.size() >= 3U && letter(path[0]) && path[1] == L':' &&
        path[2] == L'\\') {
        return true;
    }
    if (path.size() >= 7U && path[0] == L'\\' && path[1] == L'\\' &&
        path[2] == L'?' && path[3] == L'\\' && letter(path[4]) &&
        path[5] == L':' && path[6] == L'\\') {
        return true;
    }
    return false;
}

}  // namespace dtv::userland::host_listener_context::detail::windows_path

#endif  // DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_WINDOWS_PATH_H
