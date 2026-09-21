// These drive the real Bridge over an in-process transport. No network, but
// they are what proves the three fixes over the original single-file bridge:
// a bounded pool, one server session, and a DELETE that cannot race in-flight
// requests.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mcpwinauth/bridge.hpp"
#include "support/fake_transport.hpp"

using namespace mcpwinauth;
using namespace mcpwinauth::testing;
using namespace std::chrono_literals;

namespace {

struct Harness {
    // Declaration order is destruction order in reverse: the bridge must die
    // first, while the sink's mutex and the fake it talks to are still alive.
    std::mutex                     mx;
    std::vector<std::string>       emitted;
    std::unique_ptr<FakeTransport> owned{new FakeTransport()};
    FakeTransport*                 fake = owned.get();
    std::unique_ptr<Bridge>        bridge;

    explicit Harness(Config cfg) {
        MessageSink out = [this](const std::string& m) {
            std::lock_guard<std::mutex> lock(mx);
            emitted.push_back(m);
        };
        bridge.reset(new Bridge(std::move(cfg), out, nullptr,
                                std::unique_ptr<ITransport>(new TransportRef(fake))));
    }

    std::vector<std::string> Emitted() {
        std::lock_guard<std::mutex> lock(mx);
        return emitted;
    }
};

Config BaseConfig(unsigned workers = 4) {
    Config cfg;
    cfg.url         = L"https://server/mcp";
    cfg.workerCount = workers;
    return cfg;
}

const char* kReq1 = R"({"jsonrpc":"2.0","id":1,"method":"initialize"})";
const char* kReq2 = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})";

}  // namespace

TEST_CASE("Start with an injected transport needs no reachable server") {
    Harness h(BaseConfig());
    CHECK(static_cast<bool>(h.bridge->Start()));
    h.bridge->Shutdown();
}

TEST_CASE("a successful POST emits the response body") {
    Harness h(BaseConfig(1));
    h.fake->replies = {Reply{200, "application/json", "sess-1",
                             R"({"jsonrpc":"2.0","id":1,"result":{}})", 0}};
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    REQUIRE(h.bridge->Submit(kReq1));
    h.bridge->Shutdown();

    const auto emitted = h.Emitted();
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0] == R"({"jsonrpc":"2.0","id":1,"result":{}})");
    CHECK(h.bridge->SessionId() == "sess-1");
    CHECK(h.bridge->GetStats().completed == 1);
}

TEST_CASE("an SSE response is framed into separate messages") {
    Harness h(BaseConfig(1));
    h.fake->replies = {Reply{200, "text/event-stream; charset=utf-8", "s",
                             "data: {\"a\":1}\n\ndata: {\"b\":2}\n\n", 0}};
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    REQUIRE(h.bridge->Submit(kReq1));
    h.bridge->Shutdown();

    const auto emitted = h.Emitted();
    REQUIRE(emitted.size() == 2);
    CHECK(emitted[0] == R"({"a":1})");
    CHECK(emitted[1] == R"({"b":2})");
}

TEST_CASE("the session id is carried on every later request") {
    Harness h(BaseConfig(1));
    h.fake->replies = {Reply{200, "application/json", "sess-1", "{}", 0},
                       Reply{200, "application/json", "", "{}", 0}};
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    REQUIRE(h.bridge->Submit(kReq1));
    REQUIRE(h.bridge->Submit(kReq2));
    h.bridge->Shutdown();

    const auto calls = h.fake->Calls();
    REQUIRE(calls.size() >= 2);
    CHECK(calls[0].sessionIdSent.empty());  // none to send yet
    CHECK(calls[1].sessionIdSent == "sess-1");
}

TEST_CASE("only one request is in flight until the session exists") {
    // The original bridge spawned a thread per stdin line, so a burst of
    // concurrent first requests made the server open a session for each and
    // the bridge kept only whichever landed last.
    Harness h(BaseConfig(4));
    h.fake->replies = {Reply{200, "application/json", "sess-1", "{}", 0}};
    h.fake->OpenGate();
    REQUIRE(static_cast<bool>(h.bridge->Start()));

    for (int i = 0; i < 4; ++i) REQUIRE(h.bridge->Submit(kReq1));

    REQUIRE(h.fake->WaitForCalls(1, 2s));
    std::this_thread::sleep_for(100ms);  // give any extra worker a chance to slip through
    CHECK(h.fake->CallCount() == 1);
    CHECK(h.fake->PeakInFlight() == 1);

    // Let the pioneer finish and establish the session. The gate stays shut,
    // so the other three pile up inside the transport instead of racing each
    // other to finish, and the overlap is a fact rather than a coincidence.
    h.fake->Release(1);
    REQUIRE(h.fake->WaitForCalls(4, 2s));
    CHECK(h.fake->PeakInFlight() >= 3);  // concurrent once the session is open

    h.fake->ReleaseAll();
    h.bridge->Shutdown();
}

TEST_CASE("a failed pioneer hands the role to the next request") {
    Harness h(BaseConfig(4));
    h.fake->replies = {Reply{503, "application/json", "", "", 0},
                       Reply{200, "application/json", "sess-1", "{}", 0}};
    h.fake->OpenGate();
    REQUIRE(static_cast<bool>(h.bridge->Start()));

    for (int i = 0; i < 4; ++i) REQUIRE(h.bridge->Submit(kReq1));

    REQUIRE(h.fake->WaitForCalls(1, 2s));
    h.fake->Release(1);  // let the pioneer through; it 503s

    REQUIRE(h.fake->WaitForCalls(2, 2s));
    std::this_thread::sleep_for(100ms);
    // A transient failure must not open the floodgates, or we are back to
    // several sessions.
    CHECK(h.fake->CallCount() == 2);
    CHECK(h.fake->PeakInFlight() == 1);

    h.fake->ReleaseAll();
    h.bridge->Shutdown();
}

TEST_CASE("the DELETE happens after every POST has finished") {
    Harness h(BaseConfig(4));
    h.fake->replies = {Reply{200, "application/json", "sess-1", "{}", 0}};
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    for (int i = 0; i < 12; ++i) REQUIRE(h.bridge->Submit(kReq1));
    h.bridge->Shutdown();

    const auto order = h.fake->Order();
    REQUIRE_FALSE(order.empty());
    CHECK(order.back() == HttpVerb::Delete);
    CHECK(std::count(order.begin(), order.end(), HttpVerb::Delete) == 1);
    // Nothing queued may be dropped on the way out either.
    CHECK(std::count(order.begin(), order.end(), HttpVerb::Post) == 12);
    CHECK(h.fake->Calls().back().sessionIdSent == "sess-1");
}

TEST_CASE("no DELETE is sent when the server never issued a session") {
    Harness h(BaseConfig(1));
    h.fake->replies = {Reply{200, "application/json", "", "{}", 0}};
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    REQUIRE(h.bridge->Submit(kReq1));
    h.bridge->Shutdown();

    const auto order = h.fake->Order();
    CHECK(std::count(order.begin(), order.end(), HttpVerb::Delete) == 0);
}

TEST_CASE("--no-delete-session is honoured") {
    Config cfg                   = BaseConfig(1);
    cfg.deleteSessionOnShutdown  = false;
    Harness h(cfg);
    h.fake->replies = {Reply{200, "application/json", "sess-1", "{}", 0}};
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    REQUIRE(h.bridge->Submit(kReq1));
    h.bridge->Shutdown();

    const auto order = h.fake->Order();
    CHECK(std::count(order.begin(), order.end(), HttpVerb::Delete) == 0);
}

TEST_CASE("Submit blocks once the queue is full") {
    Config cfg         = BaseConfig(1);
    cfg.maxQueueDepth  = 2;
    Harness h(cfg);
    h.fake->replies = {Reply{200, "application/json", "sess-1", "{}", 0}};
    h.fake->OpenGate();
    REQUIRE(static_cast<bool>(h.bridge->Start()));

    std::atomic<int> accepted{0};
    std::thread      producer([&] {
        for (int i = 0; i < 6; ++i) {
            h.bridge->Submit(kReq1);
            accepted.fetch_add(1);
        }
    });

    std::this_thread::sleep_for(200ms);
    // One worker holds a message, two more fit in the queue; the rest must
    // wait rather than be dropped.
    CHECK(accepted.load() < 6);

    h.fake->ReleaseAll();
    producer.join();
    CHECK(accepted.load() == 6);
    h.bridge->Shutdown();
}

TEST_CASE("a failed request answers the client instead of leaving it hanging") {
    SUBCASE("HTTP error on a request with an id") {
        Harness h(BaseConfig(1));
        h.fake->replies = {Reply{500, "application/json", "", "", 0}};
        REQUIRE(static_cast<bool>(h.bridge->Start()));
        REQUIRE(h.bridge->Submit(kReq1));
        h.bridge->Shutdown();

        const auto emitted = h.Emitted();
        REQUIRE(emitted.size() == 1);
        CHECK(emitted[0].find("\"id\":1") != std::string::npos);
        CHECK(emitted[0].find("-32603") != std::string::npos);
        CHECK(emitted[0].find("HTTP 500") != std::string::npos);
        CHECK(h.bridge->GetStats().failed == 1);
    }
    SUBCASE("transport failure") {
        Harness h(BaseConfig(1));
        h.fake->replies = {Reply{0, "", "", "", 12029}};
        REQUIRE(static_cast<bool>(h.bridge->Start()));
        REQUIRE(h.bridge->Submit(kReq1));
        h.bridge->Shutdown();

        const auto emitted = h.Emitted();
        REQUIRE(emitted.size() == 1);
        CHECK(emitted[0].find("12029") != std::string::npos);
    }
    SUBCASE("a failed notification gets no reply") {
        Harness h(BaseConfig(1));
        h.fake->replies = {Reply{500, "application/json", "", "", 0}};
        REQUIRE(static_cast<bool>(h.bridge->Start()));
        REQUIRE(h.bridge->Submit(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
        h.bridge->Shutdown();
        CHECK(h.Emitted().empty());
    }
}

TEST_CASE("Shutdown is idempotent and Submit stops accepting afterwards") {
    Harness h(BaseConfig(1));
    h.fake->replies = {Reply{200, "application/json", "sess-1", "{}", 0}};
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    REQUIRE(h.bridge->Submit(kReq1));

    h.bridge->Shutdown();
    h.bridge->Shutdown();
    h.bridge->Shutdown();

    CHECK_FALSE(h.bridge->Submit(kReq2));
    const auto order = h.fake->Order();
    CHECK(std::count(order.begin(), order.end(), HttpVerb::Delete) == 1);
}

TEST_CASE("destroying a running bridge shuts it down cleanly") {
    Harness h(BaseConfig(2));
    h.fake->replies = {Reply{200, "application/json", "sess-1", "{}", 0}};
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    for (int i = 0; i < 5; ++i) REQUIRE(h.bridge->Submit(kReq1));
    h.bridge.reset();  // must not hang or leak a worker
    const auto order = h.fake->Order();
    CHECK(std::count(order.begin(), order.end(), HttpVerb::Post) == 5);
}

TEST_CASE("Start twice is refused") {
    Harness h(BaseConfig(1));
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    const Error second = h.bridge->Start();
    CHECK_FALSE(static_cast<bool>(second));
    CHECK(second.status == Status::AlreadyStarted);
    h.bridge->Shutdown();
}

TEST_CASE("Submit before Start is refused") {
    Harness h(BaseConfig(1));
    CHECK_FALSE(h.bridge->Submit(kReq1));
}

TEST_CASE("a bad url is reported without a transport") {
    Config cfg = BaseConfig(1);
    cfg.url    = L"ftp://server/mcp";
    Bridge      bridge(cfg, nullptr, nullptr);  // no injected transport
    const Error e = bridge.Start();
    CHECK_FALSE(static_cast<bool>(e));
    CHECK(e.status == Status::BadUrl);
}

TEST_CASE("the drain timeout abandons a stuck request instead of hanging forever") {
    Config cfg           = BaseConfig(1);
    cfg.drainTimeoutMs   = 150;
    Harness h(cfg);
    h.fake->replies = {Reply{200, "application/json", "sess-1", "{}", 0}};
    h.fake->OpenGate();
    REQUIRE(static_cast<bool>(h.bridge->Start()));
    REQUIRE(h.bridge->Submit(kReq1));
    REQUIRE(h.fake->WaitForCalls(1, 2s));

    std::thread unstick([&] {
        std::this_thread::sleep_for(400ms);
        h.fake->ReleaseAll();  // the real transport would be cut off by Abort()
    });

    const auto start = std::chrono::steady_clock::now();
    h.bridge->Shutdown();
    unstick.join();

    CHECK(h.fake->aborted.load());
    // An abandoned session must not get a DELETE on a handle we just closed.
    const auto order = h.fake->Order();
    CHECK(std::count(order.begin(), order.end(), HttpVerb::Delete) == 0);
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}
