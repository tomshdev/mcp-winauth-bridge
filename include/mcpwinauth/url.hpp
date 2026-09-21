// URL splitting. Backed by WinHttpCrackUrl, but deterministic and free of I/O,
// so it links and runs inside the unit tests.
#pragma once

#include <cstdint>
#include <string>

namespace mcpwinauth {

struct ParsedUrl {
    std::wstring host;
    std::wstring path;  // url path plus query string
    uint16_t     port   = 0;
    bool         secure = false;
};

// Returns false and fills `win32` with GetLastError() when the URL is not a
// usable http/https URL.
bool ParseUrl(const std::wstring& url, ParsedUrl& out, unsigned long& win32);

}  // namespace mcpwinauth
