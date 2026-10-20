// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_TRANSPORT_H
#define PX4_USERLAND_TRANSPORT_H

#include "px4/error.h"

// The neutral byte-view/transport types live in the MIT dtv-userland-core
// foundation library and are aliased into px4::userland here.
#include "dtv/userland/transport.h"

namespace px4::userland {

using dtv::userland::BulkReadObservation;
using dtv::userland::ByteView;
using dtv::userland::MutableByteView;
using dtv::userland::StreamConfig;
using dtv::userland::StreamEvent;
using dtv::userland::StreamEventKind;
using dtv::userland::Timeout;
using dtv::userland::Transport;

}  // namespace px4::userland

#endif  // PX4_USERLAND_TRANSPORT_H
