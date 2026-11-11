#include "HttpClient.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>

#include "Text.h"

namespace modelhub {
namespace {

struct HInternet {
    HINTERNET h = nullptr;
    explicit HInternet(HINTERNET v) : h(v) {}
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
};

std::string WinHttpError(const std::string& what) {
    return what + " failed (" + std::to_string(GetLastError()) + ")";
}

HINTERNET SharedSession() {
    static HINTERNET session = WinHttpOpen(L"ModelHub/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                           WINHTTP_NO_PROXY_BYPASS, 0);
    return session;
}

void ReadHeaders(HINTERNET req, HttpResponse& r) {
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);
    DWORD rawLen = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &rawLen,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !rawLen) return;
    std::wstring raw(rawLen / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &rawLen,
                             WINHTTP_NO_HEADER_INDEX))
        return;
    for (const auto& line : SplitLines(Narrow(raw))) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        r.headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
    }
}

}  // namespace

HttpResponse HttpRequestStream(const std::string& method, const std::string& url, const std::string& body,
                               const HttpHeaders& headers, int timeoutMs, const HttpChunkFn& onChunk,
                               std::stop_token stop, const HttpHeadersFn& onHeaders) {
    HttpResponse r;
    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        r.error = "invalid URL: " + url;
        return r;
    }
    HINTERNET session = SharedSession();
    if (!session) {
        r.error = WinHttpError("WinHttpOpen");
        return r;
    }
    HInternet conn(WinHttpConnect(session, host, uc.nPort, 0));
    if (!conn.h) {
        r.error = WinHttpError("WinHttpConnect");
        return r;
    }
    std::wstring target = std::wstring(path) + extra;
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn.h, Widen(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        r.error = WinHttpError("WinHttpOpenRequest");
        return r;
    }
    // Closing the request handle is WinHTTP's documented way to abort a blocked synchronous call.
    std::atomic<HINTERNET> live{req};
    auto closeOnce = [&live] {
        if (HINTERNET h = live.exchange(nullptr)) WinHttpCloseHandle(h);
    };
    std::stop_callback onStop(stop, closeOnce);
    struct Closer {
        decltype(closeOnce)& f;
        ~Closer() { f(); }
    } closer{closeOnce};

    WinHttpSetTimeouts(req, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    std::wstring hdrs;
    for (const auto& [k, v] : headers) hdrs += Widen(k) + L": " + Widen(v) + L"\r\n";
    if (!WinHttpSendRequest(req, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                            hdrs.empty() ? 0 : static_cast<DWORD>(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        r.cancelled = stop.stop_requested();
        r.error = r.cancelled ? "cancelled" : WinHttpError("HTTP " + method + " " + url);
        return r;
    }
    ReadHeaders(req, r);
    if (onHeaders && !onHeaders(r)) {
        r.cancelled = true;
        r.error = "aborted after headers (HTTP " + std::to_string(r.status) + ")";
        return r;
    }

    std::string buf;
    for (;;) {
        if (stop.stop_requested()) {
            r.cancelled = true;
            r.error = "cancelled";
            break;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            r.cancelled = stop.stop_requested();
            r.error = r.cancelled ? "cancelled" : WinHttpError("WinHttpQueryDataAvailable");
            break;
        }
        if (avail == 0) break;
        buf.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &read)) {
            r.cancelled = stop.stop_requested();
            r.error = r.cancelled ? "cancelled" : WinHttpError("WinHttpReadData");
            break;
        }
        if (read && onChunk && !onChunk(std::string_view(buf.data(), read))) {
            r.cancelled = true;
            r.error = "cancelled";
            break;
        }
    }
    return r;
}

HttpResponse HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                         const HttpHeaders& headers, int timeoutMs) {
    std::string collected;
    HttpResponse r = HttpRequestStream(method, url, body, headers, timeoutMs, [&](std::string_view chunk) {
        collected.append(chunk);
        return true;
    });
    r.body = std::move(collected);
    return r;
}

std::vector<std::string> ParseSseEvents(const std::string& body) {
    std::vector<std::string> events;
    std::string data;
    bool has = false;
    for (const auto& line : SplitLines(body)) {
        if (line.empty()) {
            if (has) events.push_back(data);
            data.clear();
            has = false;
            continue;
        }
        if (line.rfind("data:", 0) == 0) {
            std::string v = line.substr(5);
            if (!v.empty() && v[0] == ' ') v.erase(0, 1);
            if (has) data += "\n";
            data += v;
            has = true;
        }
    }
    if (has) events.push_back(data);
    return events;
}

}  // namespace modelhub
