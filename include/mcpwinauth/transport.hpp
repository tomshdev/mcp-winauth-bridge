// The seam between the bridge's logic and the network. Everything above this
// interface is testable without a server; WinHttpTransport is the only thing
// below it.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mcpwinauth/config.hpp"
#include "mcpwinauth/url.hpp"

namespace mcpwinauth {

enum class HttpVerb { Post, Delete };

struct ResponseHead {
    unsigned long statusCode = 0;  // 0 means the request never got a response
    unsigned long win32Error = 0;
    std::string   contentType;     // lowercased
    std::string   sessionId;       // Mcp-Session-Id, "" when absent
};

// Body bytes in arrival order, on the calling thread.
using BodyChunkFn = std::function<void(const char* data, size_t len)>;
using HeadFn      = std::function<void(const ResponseHead&)>;

class ITransport {
public:
    virtual ~ITransport() = default;

    // Blocking. `onHead` runs once before any chunk, and only if headers
    // arrived. The return value repeats the head for convenience.
    virtual ResponseHead Request(HttpVerb           verb,
                                 const std::string& body,
                                 const std::string& sessionId,
                                 const HeadFn&      onHead,
                                 const BodyChunkFn& onChunk) = 0;

    // Abandon every in-flight request. The only safe way to unblock a thread
    // parked inside the network stack.
    virtual void Abort() = 0;
};

Error MakeWinHttpTransport(const Config&                cfg,
                           const ParsedUrl&             target,
                           LogSink                      log,
                           std::unique_ptr<ITransport>& out);

}  // namespace mcpwinauth
