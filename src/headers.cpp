#include "mcpwinauth/headers.hpp"

namespace mcpwinauth {
namespace {

// A generous ceiling. Long enough for any real session id, short enough that
// a hostile server cannot make us build an enormous header block.
const size_t kMaxSessionIdLength = 512;

bool IsVisibleAscii(unsigned char c) {
    return c >= 0x21 && c <= 0x7E;
}

}  // namespace

bool IsValidSessionId(const std::string& value) {
    if (value.empty() || value.size() > kMaxSessionIdLength) return false;
    for (const char c : value)
        if (!IsVisibleAscii(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string SanitizeHeaderValue(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        out += (IsVisibleAscii(u) || u == ' ' || u == '\t') ? c : '?';
    }
    return out;
}

}  // namespace mcpwinauth
