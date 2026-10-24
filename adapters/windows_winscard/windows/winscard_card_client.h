// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_WINSCARD_CARD_CLIENT_H
#define PX4_USERLAND_WINSCARD_CARD_CLIENT_H

#include "px4/ipc.h"
#include "px4/transport.h"

#include <dtv/userland/control_client.h>

#include <memory>
#include <string>
#include <vector>

namespace px4::userland::windows_card {

struct Reader final {
    std::string product;
    std::string instance;
    std::uint16_t reader_id = 0U;
    std::string display_name;
};

struct ReaderStatus final {
    bool present = false;
    bool initialized = false;
    std::uint64_t generation = 0U;
    std::vector<std::uint8_t> atr;
};

// runtime_root is the UTF-8 local-absolute runtime parent (normally
// LOCALAPPDATA), not its product child. Only already-running private endpoints
// under px4-userland/v1 and asicen-userland/v1 that advertise CARD_SERVICE
// with a ready context are returned. Discovery is bounded across both products.
Result<std::vector<Reader>> list_readers(const std::string& runtime_root) noexcept;

// One current snapshot, not an idle wait. The shared CARD_STATUS reads the
// reader's present/initialized/ATR without connecting, initializing the card
// protocol, resetting, or sending an APDU.
Result<ReaderStatus> reader_status(const std::string& runtime_root,
                                  const std::string& product,
                                  const std::string& instance,
                                  std::uint16_t reader_id) noexcept;

class CardClient final {
public:
    static Result<std::unique_ptr<CardClient>> connect(
        const std::string& runtime_root, const std::string& product,
        const std::string& instance, std::uint16_t reader_id,
        ipc::ShareMode mode) noexcept;
    ~CardClient() noexcept;
    CardClient(const CardClient&) = delete;
    CardClient& operator=(const CardClient&) = delete;
    // No implicit reconnect or retry. Ambiguous I/O and observed card loss
    // permanently close this connection; the session handle never leaves this
    // type.
    Result<std::vector<std::uint8_t>> transmit(ByteView apdu) noexcept;
    Result<ReaderStatus> status() noexcept;
    Result<void> begin_transaction() noexcept;
    Result<void> end_transaction(ipc::Disposition disposition) noexcept;
    Result<void> disconnect(ipc::Disposition disposition) noexcept;

private:
    CardClient(std::unique_ptr<dtv::userland::wire::TimedClientTransport> transport,
               dtv::userland::wire::CardClientSession session,
               std::vector<std::uint8_t> atr) noexcept
        : transport_(std::move(transport)), session_(session), atr_(std::move(atr)) {}
    // Permanently drops the owning connection after a terminal error or a
    // CARD_DISCONNECT; no request is ever replayed on a fresh connection.
    void forget() noexcept;
    std::unique_ptr<dtv::userland::wire::TimedClientTransport> transport_;
    dtv::userland::wire::CardClientSession session_;
    std::vector<std::uint8_t> atr_;
};

}  // namespace px4::userland::windows_card

#endif  // PX4_USERLAND_WINSCARD_CARD_CLIENT_H
