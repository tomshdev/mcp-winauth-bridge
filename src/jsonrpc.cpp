#include "mcpwinauth/jsonrpc.hpp"

#include <cctype>
#include <cstdio>

namespace mcpwinauth {
namespace {

const char kQuote     = '"';
const char kBackslash = '\\';

// Advances `i` from the opening quote of a JSON string to its closing quote.
// Returns false if the string never terminates.
bool SkipString(const std::string& j, size_t& i) {
    ++i;  // past the opening quote
    while (i < j.size()) {
        if (j[i] == kBackslash) {
            if (i + 1 >= j.size()) return false;
            i += 2;
            continue;
        }
        if (j[i] == kQuote) return true;
        ++i;
    }
    return false;
}

// Finds the value of a member named `key` at nesting depth 1 and returns the
// span of its raw token. Members of nested objects, and text inside string
// values, are skipped, which a plain find() would not do.
bool FindTopLevelMember(const std::string& j, const char* key, size_t keyLen,
                        size_t& valueBegin, size_t& valueEnd) {
    int depth = 0;
    for (size_t i = 0; i < j.size(); ++i) {
        const char c = j[i];
        if (c == kQuote) {
            const size_t nameBegin = i;
            if (!SkipString(j, i)) return false;  // malformed
            const size_t nameLen = i - nameBegin + 1;
            if (depth != 1 || nameLen != keyLen || j.compare(nameBegin, keyLen, key) != 0)
                continue;

            size_t k = j.find(':', i + 1);
            if (k == std::string::npos) return false;
            ++k;
            while (k < j.size() && std::isspace(static_cast<unsigned char>(j[k]))) ++k;
            if (k >= j.size()) return false;

            size_t e = k;
            if (j[e] == kQuote) {
                if (!SkipString(j, e)) return false;
                ++e;  // past the closing quote
            } else {
                while (e < j.size() && j[e] != ',' && j[e] != '}' &&
                       !std::isspace(static_cast<unsigned char>(j[e])))
                    ++e;
            }
            valueBegin = k;
            valueEnd   = e;
            return true;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return false;
}

}  // namespace

std::string ToSingleLine(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n') c = ' ';
    return s;
}

bool IsBlank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string TopLevelId(const std::string& json) {
    size_t b = 0, e = 0;
    if (!FindTopLevelMember(json, "\"id\"", 4, b, e)) return std::string();
    return json.substr(b, e - b);
}

bool IsRequest(const std::string& json) {
    size_t b = 0, e = 0;
    return FindTopLevelMember(json, "\"method\"", 8, b, e);
}

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string MakeErrorResponse(const std::string& rawId, int code, const std::string& message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + rawId + ",\"error\":{\"code\":" +
           std::to_string(code) + ",\"message\":\"" + EscapeJsonString(message) + "\"}}";
}

std::string MakeFailureReply(const std::string& request, const std::string& message) {
    if (!IsRequest(request)) return std::string();  // a response, or not JSON-RPC
    const std::string id = TopLevelId(request);
    if (id.empty() || id == "null") return std::string();  // a notification wants no reply
    return MakeErrorResponse(id, -32603, message);
}

}  // namespace mcpwinauth
