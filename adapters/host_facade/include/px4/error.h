// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_ERROR_H
#define PX4_USERLAND_ERROR_H

// Error/Result are provided by the MIT dtv-userland-core foundation library.
// The product keeps the px4::userland names as aliases so existing sources and
// tests are unchanged.
#include "dtv/userland/error.h"

namespace px4::userland {

using dtv::userland::Error;
using dtv::userland::error_string;
using dtv::userland::Result;

}  // namespace px4::userland

#endif  // PX4_USERLAND_ERROR_H
