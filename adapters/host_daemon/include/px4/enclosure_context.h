// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_ENCLOSURE_CONTEXT_H
#define PX4_USERLAND_ENCLOSURE_CONTEXT_H

#include "px4/card_service.h"
#include "px4/control_server.h"
#include "px4/posix_ipc.h"
#include "px4/tuner_service.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace px4::userland::ipc::posix {

struct EnclosureCleanupThread;

// The product hardware graph of one enclosure: runtime, transports and their
// internal descriptor duplicates, controllers, frontend, power and LNB
// coordinators, card backend/protocol, the tuner backend, the stream data plane
// and the clock they share.  It is destroyed as one unit, after every service,
// worker and server that can reach it.  Implementations order their own
// members so that dependents are destroyed before what they reference.
class EnclosureFamily {
public:
    virtual ~EnclosureFamily() noexcept = default;
    virtual Result<void> shutdown() noexcept { return Result<void>::success(); }
    virtual TunerServiceBackend& tuner_backend() noexcept = 0;
    virtual CardServiceBackend& card_backend() noexcept = 0;
    virtual CardProtocolSession& card_protocol() noexcept = 0;
    virtual TunerServiceTime& time() noexcept = 0;
    // Null when the family has no data plane.
    virtual TunerStreamControl* stream_control() noexcept = 0;
};

// Endpoint namespace of a published context.  The strings are owned copies.
struct EnclosurePublication final {
    // Empty selects the adapter default.
    std::string runtime_directory;
    std::string instance;
    // Identity of the serial namespace lease (SerialEndpointLease::acquire).
    std::string lease_identity;
    EndpointAccess access = EndpointAccess::private_user;
};

// Outcome of one close() call.  quiesced and error are independent: a cleanup
// may fail and still quiesce (every task returned and the graph was released),
// and a context that has not quiesced reports no completed cleanup at all.
struct ContextCloseResult final {
    // Every worker task and callback returned, the server and the service
    // shutdowns finished, and the whole graph was released in reverse
    // dependency order, including the namespace lease.
    bool quiesced = false;
    // The budget of this call elapsed first.  The graph is retained.
    bool deadline_expired = false;
    // Primary error of the whole-context cleanup; meaningful once quiesced.
    Error error = Error::OK;
};

// Owner of one enclosure's whole object graph: the family above, the tuner
// and card services, the namespace lease, and the control server with its
// worker lanes and wake handles.  It is the single lifetime authority; the
// typed engine stays the single state authority.
//
// Serialization: one serving thread calls publish() and poll_once().  close()
// marks the context closing without waiting for the serving thread; cleanup
// waits for an in-progress poll before touching the server.
//
// Whole-context close: the first close() issues the cleanup exactly once to a
// private cleanup thread created by assemble().  That thread alone performs
// the blocking work (server shutdown, worker join, service shutdown and every
// destructor).  close() only waits, up to its finite budget, for the thread to
// report quiescence; it takes no lock the cleanup holds, performs no USB or
// diagnostic I/O and never ends the process.  If the budget elapses the graph,
// its locks, descriptors and namespace lease are retained and the cleanup
// continues; a later close() observes the same retained cleanup.
//
// Embedded hosts keep the context while it is not quiesced, keep every host
// object the family borrows alive, and keep this module loaded.  Destroying a
// context that has not quiesced does not free its graph early: the destructor
// waits for the retained cleanup.  A standalone daemon applies its own policy
// to an expired budget; this class never does.
class EnclosureContext final {
public:
    // Builds the card and tuner services over the family and starts the idle
    // cleanup thread.  Strict tune capabilities are configured here.
    static Result<std::unique_ptr<EnclosureContext>> assemble(
        std::unique_ptr<EnclosureFamily> family,
        std::unique_ptr<TunerNonceSource> nonce_source) noexcept;

    ~EnclosureContext() noexcept;
    EnclosureContext(const EnclosureContext&) = delete;
    EnclosureContext& operator=(const EnclosureContext&) = delete;

    TunerService& tuner_service() noexcept { return *tuner_; }
    CardService& card_service() noexcept { return *card_; }

    // Acquires the namespace lease for publication.  Once, before publish().
    Result<void> reserve_namespace(const EnclosurePublication& publication) noexcept;
    // Adopts a lease acquired earlier in product startup.  Once, before publish().
    Result<void> adopt_namespace(const EnclosurePublication& publication,
                                 SerialEndpointLease lease) noexcept;
    // Creates the control server in the reserved namespace.  Once.
    Result<void> publish(const ControlContext& context) noexcept;
    const PathChar* endpoint_path() const noexcept;

    // One foreground poll; NOT_READY once close() has started.
    Result<void> poll_once(Timeout timeout) noexcept;

    // Upper bound accepted for one close() budget.
    static constexpr std::chrono::milliseconds kMaxCloseBudget{3600000};
    // Starts the whole-context cleanup once and waits at most budget (clamped
    // to kMaxCloseBudget) for it to quiesce.
    ContextCloseResult close(std::chrono::milliseconds budget) noexcept;

private:
    EnclosureContext() noexcept = default;
    static void* cleanup_entry(void* context) noexcept;
    void run_cleanup() noexcept;

    // Declaration order is the reverse of release order: the lease and the
    // publication strings outlive the family, the family outlives the
    // services, and the services outlive the server.
    SerialEndpointLease lease_;
    EnclosurePublication publication_{};
    std::unique_ptr<EnclosureFamily> family_;
    std::unique_ptr<TunerNonceSource> nonce_;
    std::unique_ptr<CardService> card_;
    std::unique_ptr<TunerService> tuner_;
    std::unique_ptr<PosixControlServer> server_;

    // Serving-thread exclusion between setup and poll_once(); close() signals
    // closing before cleanup waits for this mutex.
    std::mutex poll_mutex_;
    std::atomic<bool> closing_{false};

    // Cleanup handshake.  The cleanup thread holds state_mutex_ only to read
    // the request and to publish its result, never across blocking work.
    std::mutex state_mutex_;
    std::condition_variable state_changed_;
    bool cleanup_requested_ = false;
    bool quiesced_ = false;
    Error cleanup_error_ = Error::OK;
    // Platform thread handle; joined by the destructor, never detached.
    std::unique_ptr<EnclosureCleanupThread> thread_;
};

}  // namespace px4::userland::ipc::posix

#endif  // PX4_USERLAND_ENCLOSURE_CONTEXT_H
