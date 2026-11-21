#pragma once
#include "stdafx.h"

namespace chat
{

// Cancels a running request from another thread (closes its WinHTTP handle) and tells loops to stop.
class CancelToken
{
public:
    void Cancel();
    bool IsCancelled() const
    {
        return cancelled_.load();
    }
    // Used by HttpSend: false when already cancelled.
    bool Attach(HINTERNET request);
    // True when the caller still owns the handle (and must close it).
    bool Detach(HINTERNET request);

private:
    std::atomic<bool> cancelled_{ false };
    std::mutex mu_;
    HINTERNET active_ = nullptr;
};

struct HttpResult
{
    int status = 0;  // 0: no response
    std::string body;
    std::string error;
    std::map<std::string, std::string> headers;  // lower-case names
    bool cancelled = false;
    bool Ok() const
    {
        return status >= 200 && status < 300;
    }
    std::string Header(const std::string& lowerName) const
    {
        auto it = headers.find(lowerName);
        return it == headers.end() ? std::string() : it->second;
    }
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;
using ChunkFn = std::function<bool(const char* data, size_t size)>;  // false: stop reading

// http:// and https:// URLs. With onChunk, a 2xx body is passed to it as it arrives instead of being kept.
HttpResult HttpSend(const wchar_t* method, const std::string& url, const std::string& body, const HttpHeaders& headers,
    DWORD receiveTimeoutMs, const ChunkFn& onChunk = {}, CancelToken* cancel = nullptr);

// Server-sent events: Feed() calls onEvent(event, data) for each complete event; false from onEvent stops.
class SseParser
{
public:
    using EventFn = std::function<bool(const std::string& event, const std::string& data)>;
    bool Feed(const char* data, size_t size, const EventFn& onEvent);
    bool Finish(const EventFn& onEvent);  // dispatches a last event without a trailing blank line

private:
    bool Line(std::string line, const EventFn& onEvent);
    std::string buf_, event_, data_;
    bool hasData_ = false;
};

}  // namespace chat
