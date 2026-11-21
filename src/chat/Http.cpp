#include "Http.h"

#include "Util.h"

#pragma comment(lib, "winhttp.lib")

namespace chat
{

void CancelToken::Cancel()
{
    cancelled_ = true;
    std::lock_guard lk(mu_);
    if (active_)
    {
        WinHttpCloseHandle(active_);
        active_ = nullptr;
    }
}

bool CancelToken::Attach(HINTERNET request)
{
    std::lock_guard lk(mu_);
    if (cancelled_)
    {
        return false;
    }

    active_ = request;
    return true;
}

bool CancelToken::Detach(HINTERNET request)
{
    std::lock_guard lk(mu_);
    if (active_ != request)
    {
        return false;  // Cancel() closed it
    }

    active_ = nullptr;
    return true;
}

static void ParseHeaders(const std::wstring& raw, std::map<std::string, std::string>& out)
{
    std::string text = Utf8(raw);
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find("\r\n", pos);
        if (eol == std::string::npos)
        {
            eol = text.size();
        }

        std::string line = text.substr(pos, eol - pos);
        pos = eol + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }

        out[Lower(TrimCopy(line.substr(0, colon)))] = TrimCopy(line.substr(colon + 1));
    }
}

HttpResult HttpSend(const wchar_t* method, const std::string& url, const std::string& body, const HttpHeaders& headers,
    DWORD receiveTimeoutMs, const ChunkFn& onChunk, CancelToken* cancel)
{
    HttpResult r;
    std::wstring wurl = Wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
    {
        r.error = "invalid URL: " + url;
        return r;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty())
    {
        path = L"/";
    }

    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    bool local = host == L"127.0.0.1" || host == L"localhost" || host == L"[::1]" || host == L"::1";

    HINTERNET session = WinHttpOpen(L"ModelHubChat/1",
        local ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        r.error = "WinHttpOpen failed";
        return r;
    }

    WinHttpSetTimeouts(session, 10000, 10000, 60000, static_cast<int>(receiveTimeoutMs));
    HINTERNET conn = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    if (!req)
    {
        r.error = "cannot connect to " + Utf8(host);
        if (conn)
        {
            WinHttpCloseHandle(conn);
        }

        WinHttpCloseHandle(session);
        return r;
    }

    bool attached = cancel && cancel->Attach(req);
    if (cancel && !attached)
    {
        r.cancelled = true;
        r.error = "cancelled";
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return r;
    }

    std::wstring hdr;
    for (const auto& [k, v] : headers)
    {
        hdr += Wide(k) + L": " + Wide(v) + L"\r\n";
    }

    BOOL sent = WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
        hdr.empty() ? 0 : static_cast<DWORD>(-1),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);
    if (sent && WinHttpReceiveResponse(req, nullptr))
    {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &size, WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);

        DWORD len = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
            &len, WINHTTP_NO_HEADER_INDEX);
        if (len > 0)
        {
            std::wstring raw(len / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &len,
                    WINHTTP_NO_HEADER_INDEX))
            {
                raw.resize(len / sizeof(wchar_t));
                ParseHeaders(raw, r.headers);
            }
        }

        std::vector<char> buf(64 * 1024);
        for (;;)
        {
            DWORD got = 0;
            if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &got))
            {
                r.error = "connection lost (error " + std::to_string(GetLastError()) + ")";
                break;
            }

            if (got == 0)
            {
                break;
            }

            if (onChunk && r.Ok())
            {
                if (!onChunk(buf.data(), got))
                {
                    r.cancelled = true;
                    break;
                }
            }
            else
            {
                r.body.append(buf.data(), got);
            }
        }
    }
    else
    {
        DWORD e = GetLastError();
        r.error = e == ERROR_WINHTTP_CANNOT_CONNECT ? "cannot connect to " + url
                  : e == ERROR_WINHTTP_TIMEOUT      ? "timed out"
                                                    : "request failed (error " + std::to_string(e) + ")";
    }
    if (cancel && cancel->IsCancelled())
    {
        r.cancelled = true;
        r.error = "cancelled";
    }

    if (!cancel || cancel->Detach(req))
    {
        WinHttpCloseHandle(req);
    }

    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return r;
}

bool SseParser::Line(std::string line, const EventFn& onEvent)
{
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }

    if (line.empty())
    {
        bool go = true;
        if (hasData_)
        {
            go = onEvent(event_, data_);
        }

        event_.clear();
        data_.clear();
        hasData_ = false;
        return go;
    }

    if (line[0] == ':')
    {
        return true;  // comment / keep-alive
    }

    size_t colon = line.find(':');
    std::string field = line.substr(0, colon), value;
    if (colon != std::string::npos)
    {
        value = line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ')
        {
            value.erase(0, 1);
        }
    }

    if (field == "data")
    {
        if (hasData_)
        {
            data_ += '\n';
        }

        data_ += value;
        hasData_ = true;
    }
    else if (field == "event")
    {
        event_ = value;
    }

    return true;
}

bool SseParser::Feed(const char* data, size_t size, const EventFn& onEvent)
{
    buf_.append(data, size);
    size_t start = 0;
    for (;;)
    {
        size_t nl = buf_.find('\n', start);
        if (nl == std::string::npos)
        {
            break;
        }

        if (!Line(buf_.substr(start, nl - start), onEvent))
        {
            buf_.clear();
            return false;
        }

        start = nl + 1;
    }

    buf_.erase(0, start);
    return true;
}

bool SseParser::Finish(const EventFn& onEvent)
{
    if (!buf_.empty())
    {
        std::string rest;
        rest.swap(buf_);
        if (!Line(rest, onEvent))
        {
            return false;
        }
    }

    return Line("", onEvent);
}

}  // namespace chat
