// SPDX-License-Identifier: MIT
#include "dtv/userland/error.h"

namespace dtv::userland {

const char* error_string(Error error) noexcept
{
    switch (error) {
    case Error::OK:
        return "OK";
    case Error::INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case Error::VERSION_MISMATCH:
        return "VERSION_MISMATCH";
    case Error::NOT_FOUND:
        return "NOT_FOUND";
    case Error::BUSY:
        return "BUSY";
    case Error::NOT_READY:
        return "NOT_READY";
    case Error::TIMEOUT:
        return "TIMEOUT";
    case Error::USB_IO:
        return "USB_IO";
    case Error::DISCONNECTED:
        return "DISCONNECTED";
    case Error::PROTOCOL_ERROR:
        return "PROTOCOL_ERROR";
    case Error::FIRMWARE_REJECTED:
        return "FIRMWARE_REJECTED";
    case Error::UNSUPPORTED:
        return "UNSUPPORTED";
    case Error::NO_CARD:
        return "NO_CARD";
    case Error::CARD_REMOVED:
        return "CARD_REMOVED";
    case Error::BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case Error::SLOW_CONSUMER:
        return "SLOW_CONSUMER";
    case Error::RESOURCE_LIMIT:
        return "RESOURCE_LIMIT";
    case Error::INTERNAL:
        return "INTERNAL";
    }

    return "UNKNOWN";
}

}  // namespace dtv::userland
