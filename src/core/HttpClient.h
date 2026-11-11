#pragma once
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modelhub {

struct HttpResponse {
    int status = 0;  // 0 = transport error
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    std::string error;
    bool cancelled = false;
    bool Ok() const { return status >= 200 && status < 300; }
    std::string Header(const std::string& lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? std::string() : it->second;
    }
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// Called for each chunk of the response body as it arrives. Return false to abort.
using HttpChunkFn = std::function<bool(std::string_view chunk)>;

// Synchronous WinHTTP calls. Safe to use from many threads at once.
HttpResponse HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                         const HttpHeaders& headers, int timeoutMs);

// Called once status and headers are known, before any body chunk. Return false to abort.
using HttpHeadersFn = std::function<bool(const HttpResponse& statusAndHeaders)>;

// Streams the body to onChunk instead of buffering it (resp.body stays empty on success).
// A stop request closes the connection at once, even while a read is blocked. Redirects are followed.
HttpResponse HttpRequestStream(const std::string& method, const std::string& url, const std::string& body,
                               const HttpHeaders& headers, int timeoutMs, const HttpChunkFn& onChunk,
                               std::stop_token stop = {}, const HttpHeadersFn& onHeaders = {});

inline HttpResponse HttpGet(const std::string& url, const HttpHeaders& headers = {}, int timeoutMs = 10000) {
    return HttpRequest("GET", url, {}, headers, timeoutMs);
}
inline HttpResponse HttpPost(const std::string& url, const std::string& body, const HttpHeaders& headers,
                             int timeoutMs) {
    return HttpRequest("POST", url, body, headers, timeoutMs);
}

// Extracts the `data:` payloads of a text/event-stream body, one string per event.
std::vector<std::string> ParseSseEvents(const std::string& body);

}  // namespace modelhub
