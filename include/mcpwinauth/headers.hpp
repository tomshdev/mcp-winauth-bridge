// Validation for header values that come back from the server and are then
// echoed into later requests. Pure, so it is unit-tested directly.
#pragma once

#include <string>

namespace mcpwinauth {

// The MCP spec restricts Mcp-Session-Id to visible ASCII (0x21 to 0x7E). We
// concatenate it into a CRLF-delimited request header block, so a value
// carrying CR or LF would let a hostile server inject headers of its own.
bool IsValidSessionId(const std::string& value);

// Replaces anything that is not printable ASCII, so a header value can be
// logged or compared without carrying control characters around.
std::string SanitizeHeaderValue(const std::string& value);

}  // namespace mcpwinauth
