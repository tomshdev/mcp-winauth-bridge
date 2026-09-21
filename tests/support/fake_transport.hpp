// An in-process ITransport. Lets the pool, the session gate and the shutdown
// drain be tested without a server.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "mcpwinauth/transport.hpp"

namespace mcpwinauth {
namespace testing {

struct Call {
    HttpVerb    verb;
    std::string body;
    std::string sessionIdSent;
};

// One scripted response. Anything left at its default behaves like a plain
// 200 application/json reply.
struct Reply {
    unsigned long status      = 200;
    std::string   contentType = "application/json";
    std::string   sessionId;
    std::string   body;
    unsigned long win32Error  = 0;
};

class FakeTransport final : public ITransport {
public:
    ResponseHead Request(HttpVerb           verb,
                         const std::string& body,
                         const std::string& sessionId,
                         const HeadFn&      onHead,
                         const BodyChunkFn& onChunk) override {
        {
            std::lock_guard<std::mutex> lock(mx_);
            calls.push_back(Call{verb, body, sessionId});
            order.push_back(verb);
            ++inFlight;
            if (inFlight > peakInFlight) peakInFlight = inFlight;
            started.notify_all();
        }

        // Hold POSTs here until the test releases them, so overlap is
        // observable rather than a matter of timing luck.
        if (verb == HttpVerb::Post) {
            std::unique_lock<std::mutex> lock(mx_);
            if (gate) {
                released.wait(lock, [this] { return releaseCount > 0 || !gate; });
                if (releaseCount > 0) --releaseCount;
            }
        }

        Reply reply;
        {
            std::lock_guard<std::mutex> lock(mx_);
            if (nextReply < replies.size()) reply = replies[nextReply++];
            else if (!replies.empty())      reply = replies.back();
        }

        ResponseHead head;
        head.statusCode  = reply.status;
        head.win32Error  = reply.win32Error;
        head.contentType = reply.contentType;
        head.sessionId   = reply.sessionId;

        if (reply.status != 0) {
            if (onHead) onHead(head);
            if (onChunk && !reply.body.empty())
                onChunk(reply.body.data(), reply.body.size());
        }

        {
            std::lock_guard<std::mutex> lock(mx_);
            --inFlight;
            finished.notify_all();
        }
        return head;
    }

    void Abort() override { aborted = true; }

    // --- test controls -------------------------------------------------

    // Hold every POST inside Request() until Release() is called.
    void OpenGate() {
        std::lock_guard<std::mutex> lock(mx_);
        gate = true;
    }

    void Release(int n = 1) {
        {
            std::lock_guard<std::mutex> lock(mx_);
            releaseCount += n;
        }
        released.notify_all();
    }

    void ReleaseAll() {
        {
            std::lock_guard<std::mutex> lock(mx_);
            gate = false;
        }
        released.notify_all();
    }

    // Waits until at least `n` calls have entered Request().
    bool WaitForCalls(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mx_);
        return started.wait_for(lock, timeout, [&] { return calls.size() >= n; });
    }

    size_t CallCount() {
        std::lock_guard<std::mutex> lock(mx_);
        return calls.size();
    }

    std::vector<Call> Calls() {
        std::lock_guard<std::mutex> lock(mx_);
        return calls;
    }

    std::vector<HttpVerb> Order() {
        std::lock_guard<std::mutex> lock(mx_);
        return order;
    }

    int PeakInFlight() {
        std::lock_guard<std::mutex> lock(mx_);
        return peakInFlight;
    }

    std::mutex            mx_;
    std::vector<Reply>    replies;
    size_t                nextReply = 0;
    std::vector<Call>     calls;
    std::vector<HttpVerb> order;
    int                   inFlight     = 0;
    int                   peakInFlight = 0;
    bool                  gate         = false;
    int                   releaseCount = 0;
    std::atomic<bool>     aborted{false};

    std::condition_variable started;
    std::condition_variable finished;
    std::condition_variable released;
};

// Bridge takes ownership of its transport and destroys it during Shutdown(),
// which is right for a real network handle but would pull the FakeTransport
// out from under a test that still wants to inspect what was sent. This hands
// the bridge a forwarder instead, so the test keeps the fake alive.
class TransportRef final : public ITransport {
public:
    explicit TransportRef(FakeTransport* target) : target_(target) {}

    ResponseHead Request(HttpVerb           verb,
                         const std::string& body,
                         const std::string& sessionId,
                         const HeadFn&      onHead,
                         const BodyChunkFn& onChunk) override {
        return target_->Request(verb, body, sessionId, onHead, onChunk);
    }

    void Abort() override { target_->Abort(); }

private:
    FakeTransport* target_;
};

}  // namespace testing
}  // namespace mcpwinauth
