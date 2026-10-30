// Modified/ported for dtv-userland-core on 2026-10-30.
//
// Derived from px4-userland userland/include/px4/card_service.h's
// NativeCardProtocolSession seam and the ASICEN e2a86ca forwarder at
// 5c91e8c57b288c0f490061789829e42dbbf61b3d.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_CARD_T1_CARD_PROTOCOL_SESSION_H
#define DTV_USERLAND_HOST_CARD_T1_CARD_PROTOCOL_SESSION_H

#include "dtv/userland/host_card_t1/card_t1.h"
#include "px4/card_service.h"

#include <algorithm>
#include <cstddef>

namespace dtv::userland::host_card_t1 {

// Connects the generic T=1 session to the existing host_daemon
// CardProtocolSession seam without changing either API. ATR bytes are the only
// representation that differs between the two layers.
class CardProtocolSessionAdapter final : public px4::userland::CardProtocolSession {
public:
    explicit CardProtocolSessionAdapter(CardSession& session) noexcept
        : session_(session)
    {
    }

    Result<void> initialize() noexcept override { return session_.initialize(); }

    Result<std::size_t> transmit(ByteView apdu,
                                 MutableByteView response) noexcept override
    {
        return session_.transmit(apdu, response);
    }

    bool initialized() const noexcept override { return session_.initialized(); }

    const px4::userland::CardAtrBytes& atr() const noexcept override
    {
        const CardAtr& source = session_.atr();
        atr_.length = source.length;
        std::copy(source.bytes.begin(), source.bytes.begin() + source.length,
                  atr_.bytes.begin());
        return atr_;
    }

    void invalidate() noexcept override
    {
        session_.invalidate();
        atr_ = px4::userland::CardAtrBytes{};
    }

    void request_stop() noexcept override {}

private:
    CardSession& session_;
    mutable px4::userland::CardAtrBytes atr_{};
};

}  // namespace dtv::userland::host_card_t1

#endif  // DTV_USERLAND_HOST_CARD_T1_CARD_PROTOCOL_SESSION_H
