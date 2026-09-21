// Pure JSON-RPC helpers. No allocation-free promises, no I/O, no Windows.
// Deliberately not a JSON parser: the bridge only needs to find the top-level
// "id" and "method" of an otherwise opaque message.
#pragma once

#include <string>

namespace mcpwinauth {

// stdio MCP needs one message per line. Newlines outside JSON strings are just
// whitespace, and newlines inside strings are always escaped, so replacing them
// with spaces is safe.
std::string ToSingleLine(std::string s);

// True if the string is empty or only spaces and tabs.
bool IsBlank(const std::string& s);

// The raw top-level "id" value of a JSON-RPC message ("7", "\"abc\"", "null"),
// or "" when there is no top-level id or the message is malformed.
std::string TopLevelId(const std::string& json);

// True if the message has a top-level "method" member, i.e. it is a request or
// a notification rather than a response.
bool IsRequest(const std::string& json);

// Escapes a string for embedding as a JSON string body (without the quotes).
std::string EscapeJsonString(const std::string& s);

// A JSON-RPC error response reusing the caller's raw id token verbatim.
std::string MakeErrorResponse(const std::string& rawId, int code, const std::string& message);

// The error to emit when a request could not be delivered, or "" if the message
// needs no reply (a notification, a response, or something unparseable). This is
// what keeps the client from waiting forever on a failed request.
std::string MakeFailureReply(const std::string& request, const std::string& message);

}  // namespace mcpwinauth
