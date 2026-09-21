#include "mcpwinauth/url.hpp"

#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace mcpwinauth {

bool ParseUrl(const std::wstring& url, ParsedUrl& out, unsigned long& win32) {
    win32 = 0;
    if (url.empty()) {
        win32 = ERROR_INVALID_PARAMETER;
        return false;
    }

    // Size every component to the whole URL. Fixed buffers silently truncate
    // long paths, which is worse than failing.
    const DWORD cap = static_cast<DWORD>(url.size()) + 1;
    std::vector<wchar_t> host(cap), path(cap), extra(cap);

    URL_COMPONENTS uc = {};
    uc.dwStructSize      = sizeof(uc);
    uc.lpszHostName      = host.data();
    uc.dwHostNameLength  = cap;
    uc.lpszUrlPath       = path.data();
    uc.dwUrlPathLength   = cap;
    uc.lpszExtraInfo     = extra.data();
    uc.dwExtraInfoLength = cap;

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        win32 = GetLastError();
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
        win32 = ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
        return false;
    }

    out.host   = std::wstring(uc.lpszHostName, uc.dwHostNameLength);
    out.path   = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                 std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    out.port   = uc.nPort;
    out.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    if (out.host.empty()) {
        win32 = ERROR_WINHTTP_INVALID_URL;
        return false;
    }
    if (out.path.empty()) out.path = L"/";
    return true;
}

}  // namespace mcpwinauth
