#pragma once
// Windows C++17. Link winhttp.lib. No Qt or account credentials required.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <utility>
#pragma comment(lib, "winhttp.lib")

namespace pterosoft {
enum class LicenseResult { Authorized, StartThroughLauncher, InvalidConfiguration, ServiceUnavailable, AccessDenied };
namespace detail {
struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle &) = delete;
    InternetHandle &operator=(const InternetHandle &) = delete;
};
inline bool validTicket(const std::wstring &ticket) {
    if (ticket.size() != 43) return false;
    for (const auto c : ticket)
        if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-')) return false;
    return true;
}
inline bool authorizedResponse(unsigned long status, std::string body, unsigned long product) {
    const auto first = body.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return false;
    body = body.substr(first, body.find_last_not_of(" \r\n\t") - first + 1);
    return status == 200 && body == "\"pterosoft-game-launch:" + std::to_string(product) + "\"";
}
}

// Call once, before creating the game world or allowing gameplay. Compile the
// trusted HTTPS verification URL and WordPress product ID into YOUR game.
// Never take either value from command-line arguments or environment variables.
inline LicenseResult verifyOnlineAtLaunch(const std::wstring &verificationUrl, unsigned long productId) {
    wchar_t buffer[128]{};
    const auto count = GetEnvironmentVariableW(L"PTEROSOFT_GAME_TICKET", buffer, 128);
    SetEnvironmentVariableW(L"PTEROSOFT_GAME_TICKET", nullptr);
    const std::wstring ticket = count > 0 && count < 128 ? std::wstring(buffer, count) : std::wstring();
    SecureZeroMemory(buffer, sizeof(buffer));
    if (!detail::validTicket(ticket)) return LicenseResult::StartThroughLauncher;
    if (!productId || verificationUrl.find_first_of(L"\r\n") != std::wstring::npos) return LicenseResult::InvalidConfiguration;
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(verificationUrl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS
        || parts.dwHostNameLength == 0 || parts.dwExtraInfoLength || parts.dwUserNameLength || parts.dwPasswordLength)
        return LicenseResult::InvalidConfiguration;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    detail::InternetHandle session(WinHttpOpen(L"PterosoftGameLicense/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000)) return LicenseResult::ServiceUnavailable;
    detail::InternetHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    if (!connection.value) return LicenseResult::ServiceUnavailable;
    detail::InternetHandle request(WinHttpOpenRequest(connection.value, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    if (!request.value || !WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
        return LicenseResult::ServiceUnavailable;
    std::string asciiTicket;
    for (const auto c : ticket) asciiTicket.push_back(static_cast<char>(c));
    std::string body = "{\"product_id\":" + std::to_string(productId) + ",\"ticket\":\"" + asciiTicket + "\"}";
    SecureZeroMemory(asciiTicket.data(), asciiTicket.size());
    const bool sent = WinHttpSendRequest(request.value, L"Content-Type: application/json\r\nAccept: application/json\r\n", DWORD(-1),
        body.data(), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) != FALSE;
    SecureZeroMemory(body.data(), body.size());
    if (!sent || !WinHttpReceiveResponse(request.value, nullptr)) return LicenseResult::ServiceUnavailable;
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return LicenseResult::ServiceUnavailable;
    if (status != 200) return LicenseResult::AccessDenied;
    std::string response;
    char chunk[1024]; DWORD read = 0;
    do {
        if (!WinHttpReadData(request.value, chunk, sizeof(chunk), &read)) return LicenseResult::ServiceUnavailable;
        response.append(chunk, read);
        if (response.size() > 4096) return LicenseResult::AccessDenied;
    } while (read);
    return detail::authorizedResponse(status, std::move(response), productId) ? LicenseResult::Authorized : LicenseResult::AccessDenied;
}
}
