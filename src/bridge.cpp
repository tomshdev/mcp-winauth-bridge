// The worker pool and the session lifecycle. Knows nothing about WinHTTP: it
// only ever sees an ITransport.
#include "mcpwinauth/bridge.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "mcpwinauth/framing.hpp"
#include "mcpwinauth/jsonrpc.hpp"
#include "mcpwinauth/url.hpp"

namespace mcpwinauth {
namespace {

// Until the server has issued an Mcp-Session-Id, only one request may be in
// flight. Otherwise a burst of concurrent first requests makes the server open
// a session per request and the bridge keeps only the last one.
enum class SessionPhase { None, Establishing, Open };

enum class Role { Pioneer, Follower };

}  // namespace

struct Bridge::Impl {
    Impl(Config c, MessageSink o, LogSink l, std::unique_ptr<ITransport> t)
        : cfg(std::move(c)), out(std::move(o)), log(std::move(l)), transport(std::move(t)) {}

    Config                      cfg;
    MessageSink                 out;
    LogSink                     log;
    std::unique_ptr<ITransport> transport;
    ParsedUrl                   target;

    bool started = false;

    // Queue
    std::mutex               qmx;
    std::condition_variable  qNotEmpty;
    std::condition_variable  qNotFull;
    std::deque<std::string>  queue;
    bool                     stopping = false;
    std::vector<std::thread> workers;
    std::atomic<unsigned>    active{0};
    std::condition_variable  idleCv;

    // Session
    mutable std::mutex      smx;
    std::condition_variable sessionCv;
    SessionPhase            phase = SessionPhase::None;
    std::string             sessionId;

    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> failed{0};

    std::once_flag shutdownOnce;
    bool           aborted = false;

    void Log(LogLevel level, const std::string& text) const {
        if (log) log(level, text);
    }

    void Emit(const std::string& message) const {
        if (out && !message.empty()) out(message);
    }

    void WorkerLoop();
    void Handle(const std::string& message);

    Role AcquireSendSlot();
    void ReleaseSendSlot(Role role, bool ok);

    void DoShutdown();

    // Makes sure the Establishing phase is always left, even on an early
    // return. Stranding it there would park every other worker until shutdown.
    class SendSlot {
    public:
        SendSlot(Impl& impl, Role role) : impl_(impl), role_(role) {}
        ~SendSlot() { impl_.ReleaseSendSlot(role_, ok_); }
        void MarkOk() { ok_ = true; }

        SendSlot(const SendSlot&)            = delete;
        SendSlot& operator=(const SendSlot&) = delete;

    private:
        Impl& impl_;
        Role  role_;
        bool  ok_ = false;
    };
};

Role Bridge::Impl::AcquireSendSlot() {
    std::unique_lock<std::mutex> lock(smx);
    for (;;) {
        if (phase == SessionPhase::Open) return Role::Follower;
        if (phase == SessionPhase::None) {
            phase = SessionPhase::Establishing;
            return Role::Pioneer;
        }
        sessionCv.wait(lock);
    }
}

void Bridge::Impl::ReleaseSendSlot(Role role, bool ok) {
    if (role != Role::Pioneer) return;
    {
        std::lock_guard<std::mutex> lock(smx);
        // A pioneer that failed hands the role back, so the next waiter retries
        // alone rather than letting a burst through after one transient error.
        // Once one request has succeeded the pool runs fully concurrent, even
        // if the server issued no session id: a stateless server must not
        // serialize us forever.
        if (phase == SessionPhase::Establishing)
            phase = ok ? SessionPhase::Open : SessionPhase::None;
    }
    sessionCv.notify_all();
}

void Bridge::Impl::Handle(const std::string& message) {
    const Role role = AcquireSendSlot();
    SendSlot   slot(*this, role);

    std::string currentSession;
    {
        std::lock_guard<std::mutex> lock(smx);
        currentSession = sessionId;
    }

    std::unique_ptr<IBodyFramer> framer;
    auto onHead = [&](const ResponseHead& head) {
        if (!head.sessionId.empty()) {
            std::lock_guard<std::mutex> lock(smx);
            if (sessionId != head.sessionId) {
                sessionId = head.sessionId;
                Log(LogLevel::Debug, "session id: " + head.sessionId);
            }
        }
        framer = MakeFramer(head.contentType, out);
    };
    auto onChunk = [&](const char* data, size_t len) {
        if (framer) framer->Feed(data, len);
    };

    const ResponseHead head =
        transport->Request(HttpVerb::Post, message, currentSession, onHead, onChunk);

    const bool ok2xx = head.statusCode >= 200 && head.statusCode < 300;
    if (ok2xx && head.complete) {
        if (framer) framer->Finish();
        slot.MarkOk();
        completed.fetch_add(1);
        return;
    }

    // A truncated body is deliberately not flushed: whatever the framer is
    // still holding is a fragment, and emitting it would look like a message.
    failed.fetch_add(1);
    std::string reason;
    if (!head.statusCode)
        reason = "transport error " + std::to_string(head.win32Error);
    else if (!head.complete)
        reason = "truncated response body (win32 " + std::to_string(head.win32Error) + ")";
    else
        reason = "HTTP " + std::to_string(head.statusCode);
    Log(LogLevel::Error, "request failed: " + reason);
    // Without this the client waits forever for a reply that will never come.
    Emit(MakeFailureReply(message, "bridge: request failed (" + reason + ")"));
}

void Bridge::Impl::WorkerLoop() {
    for (;;) {
        std::string message;
        {
            std::unique_lock<std::mutex> lock(qmx);
            qNotEmpty.wait(lock, [this] { return !queue.empty() || stopping; });
            // `stopping` alone is not enough to leave: the queue must drain
            // first, so nothing already accepted is silently dropped.
            if (queue.empty()) return;
            message = std::move(queue.front());
            queue.pop_front();
            active.fetch_add(1);
        }
        qNotFull.notify_one();

        try {
            Handle(message);
        } catch (const std::exception& e) {
            failed.fetch_add(1);
            Log(LogLevel::Error, std::string("worker exception: ") + e.what());
            Emit(MakeFailureReply(message, "bridge: internal error"));
        } catch (...) {
            failed.fetch_add(1);
            Log(LogLevel::Error, "worker exception");
            Emit(MakeFailureReply(message, "bridge: internal error"));
        }

        // The decrement has to happen under qmx, not merely atomically: the
        // drain in DoShutdown evaluates its predicate while holding qmx, and
        // an unlocked decrement plus notify can slip in just before it waits,
        // costing it the wakeup and the whole drain timeout.
        {
            std::lock_guard<std::mutex> lock(qmx);
            active.fetch_sub(1);
        }
        idleCv.notify_all();
    }
}

Bridge::Bridge(Config cfg, MessageSink out, LogSink log)
    : impl_(new Impl(std::move(cfg), std::move(out), std::move(log), nullptr)) {}

Bridge::Bridge(Config cfg, MessageSink out, LogSink log, std::unique_ptr<ITransport> transport)
    : impl_(new Impl(std::move(cfg), std::move(out), std::move(log), std::move(transport))) {}

Bridge::~Bridge() {
    Shutdown();
}

Error Bridge::Start() {
    if (impl_->started) return MakeError(Status::AlreadyStarted, "bridge already started");
    if (impl_->cfg.workerCount == 0) impl_->cfg.workerCount = 1;

    // An injected transport brings its own endpoint, so the URL is only
    // required when we have to build the WinHTTP one.
    if (!impl_->transport) {
        unsigned long win32 = 0;
        if (!ParseUrl(impl_->cfg.url, impl_->target, win32))
            return MakeError(Status::BadUrl, "could not parse the MCP url", win32);

        Error e = MakeWinHttpTransport(impl_->cfg, impl_->target, impl_->log, impl_->transport);
        if (!e) return e;
    }

    impl_->started = true;
    impl_->workers.reserve(impl_->cfg.workerCount);
    for (unsigned i = 0; i < impl_->cfg.workerCount; ++i)
        impl_->workers.emplace_back([this] { impl_->WorkerLoop(); });

    impl_->Log(LogLevel::Info,
               "started with " + std::to_string(impl_->cfg.workerCount) + " workers");
    return Error{};
}

bool Bridge::Submit(std::string message) {
    if (!impl_->started) return false;

    std::unique_lock<std::mutex> lock(impl_->qmx);
    if (impl_->cfg.maxQueueDepth > 0) {
        // Blocking is deliberate: dropping a message would leave the client
        // waiting on a request/response pair that can never complete, and over
        // a pipe not reading stdin is exactly the right backpressure.
        impl_->qNotFull.wait(lock, [this] {
            return impl_->stopping || impl_->queue.size() < impl_->cfg.maxQueueDepth;
        });
    }
    if (impl_->stopping) return false;

    impl_->queue.push_back(std::move(message));
    impl_->submitted.fetch_add(1);
    lock.unlock();
    impl_->qNotEmpty.notify_one();
    return true;
}

void Bridge::Impl::DoShutdown() {
    if (!started) {
        transport.reset();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(qmx);
        stopping = true;
    }
    qNotEmpty.notify_all();
    qNotFull.notify_all();

    // Release anyone parked on the session gate, or the drain never finishes.
    {
        std::lock_guard<std::mutex> lock(smx);
        if (phase == SessionPhase::Establishing) phase = SessionPhase::Open;
    }
    sessionCv.notify_all();

    if (cfg.drainTimeoutMs > 0) {
        std::unique_lock<std::mutex> lock(qmx);
        const bool drained = idleCv.wait_for(
            lock, std::chrono::milliseconds(cfg.drainTimeoutMs),
            [this] { return queue.empty() && active.load() == 0; });
        lock.unlock();
        if (!drained) {
            Log(LogLevel::Warn, "drain timed out, abandoning in-flight requests");
            aborted = true;
            // A thread blocked in the network stack cannot be interrupted any
            // other way.
            if (transport) transport->Abort();
        }
    }

    for (std::thread& t : workers)
        if (t.joinable()) t.join();
    workers.clear();

    // Only now, with nothing in flight, is it safe to end the server session.
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(smx);
        sid = sessionId;
    }
    if (cfg.deleteSessionOnShutdown && !sid.empty() && !aborted && transport) {
        Log(LogLevel::Debug, "ending server session " + sid);
        transport->Request(HttpVerb::Delete, "", sid, nullptr, nullptr);
    }

    transport.reset();
    started = false;
}

void Bridge::Shutdown() {
    std::call_once(impl_->shutdownOnce, [this] { impl_->DoShutdown(); });
}

std::string Bridge::SessionId() const {
    std::lock_guard<std::mutex> lock(impl_->smx);
    return impl_->sessionId;
}

Bridge::Stats Bridge::GetStats() const {
    Stats s;
    s.submitted = impl_->submitted.load();
    s.completed = impl_->completed.load();
    s.failed    = impl_->failed.load();
    return s;
}

}  // namespace mcpwinauth
