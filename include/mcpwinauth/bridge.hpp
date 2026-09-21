// The bridge itself: a bounded worker pool in front of an ITransport, with the
// MCP session id shared across requests.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

#include "mcpwinauth/config.hpp"
#include "mcpwinauth/transport.hpp"

namespace mcpwinauth {

class Bridge {
public:
    Bridge(Config cfg, MessageSink out, LogSink log);

    // Injects a transport instead of building the WinHTTP one. Used by tests
    // and by hosts that already own an HTTP client.
    Bridge(Config cfg, MessageSink out, LogSink log, std::unique_ptr<ITransport> transport);

    ~Bridge();  // shuts down if the caller did not

    Bridge(const Bridge&)            = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Validates the config, parses the URL, opens the transport and starts the
    // workers.
    Error Start();

    // Queues one JSON-RPC message, without a trailing newline. Blocks while the
    // queue is full: that is the backpressure that stops us draining stdin
    // faster than the server can answer. False if stopping or not started.
    bool Submit(std::string message);

    // Drain the queue, join the workers, end the server session, close the
    // transport. Idempotent, callable from any thread.
    void Shutdown();

    // The current Mcp-Session-Id, or "" if the server has not issued one.
    std::string SessionId() const;

    struct Stats {
        uint64_t submitted = 0;
        uint64_t completed = 0;
        uint64_t failed    = 0;
    };
    Stats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The standalone program's loop: read lines until EOF, submit each, shut down.
// Returns once the session DELETE has completed.
Error RunStdioBridge(Config cfg, std::istream& in, MessageSink out, LogSink log);

// Full command-line entry point, reusable from a host exe. Returns a process
// exit code.
int BridgeMain(int argc, wchar_t** argv);

}  // namespace mcpwinauth
