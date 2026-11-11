// modelhub_client.h - header-only client for ModelHub (C++14, Windows, WinHTTP; no other dependencies).
//
//   modelhub::client::Client hub("MyApp");
//   std::string err;
//   if (hub.Connect(true, 20000, &err)) {                         // starts the hub if it is not running
//       auto advice = hub.Advise("code-conversion", "hub", "{\"lines\": 4200}", "4200 lines");   // JSON text
//       hub.StartDownload("qwen2.5-coder-7b");                    // poll hub.Downloads() for progress
//       std::wstring gguf = hub.ModelPath("qwen2.5-coder-7b");    // load it in-process, or ...
//       // ... send OpenAI requests to hub.BaseUrl() ("http://127.0.0.1:<port>/v1") with
//       //     "Authorization: Bearer " + hub.Token(); the model's llama-server starts on first use.
//   }
//
// Responses are returned as JSON text: parse them with the JSON library the application already uses.
// JsonString()/JsonNumber() below read single fields for applications without one.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace modelhub {
namespace client {

// ---------------------------------------------------------------- small helpers

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

// The first string value of "key" in a JSON text (top level or nested). Enough for hub.json and simple fields.
inline bool JsonString(const std::string& json, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    for (size_t at = json.find(pat); at != std::string::npos; at = json.find(pat, at + 1)) {
        size_t p = at + pat.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
        if (p >= json.size() || json[p] != ':') continue;
        ++p;
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
        if (p >= json.size() || json[p] != '"') continue;
        out.clear();
        for (++p; p < json.size() && json[p] != '"'; ++p) {
            char c = json[p];
            if (c != '\\' || p + 1 >= json.size()) {
                out += c;
                continue;
            }
            char e = json[++p];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (p + 4 >= json.size()) return false;
                    unsigned cp = static_cast<unsigned>(std::strtoul(json.substr(p + 1, 4).c_str(), nullptr, 16));
                    p += 4;
                    wchar_t wc = static_cast<wchar_t>(cp);
                    out += Narrow(std::wstring(1, wc));
                    break;
                }
                default: out += e;  // \" \\ \/
            }
        }
        return true;
    }
    return false;
}

inline bool JsonNumber(const std::string& json, const std::string& key, double& out) {
    std::string pat = "\"" + key + "\"";
    size_t at = json.find(pat);
    if (at == std::string::npos) return false;
    size_t p = json.find(':', at + pat.size());
    if (p == std::string::npos) return false;
    char* end = nullptr;
    out = std::strtod(json.c_str() + p + 1, &end);
    return end != json.c_str() + p + 1;
}

inline std::string UrlEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += static_cast<char>(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

// %MODELHUB_HOME%, else %LOCALAPPDATA%\ModelHub.
inline std::wstring HubHome() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"MODELHUB_HOME", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return std::wstring(buf, n);
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring(L".")) + L"\\ModelHub";
}

inline bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------- HTTP

struct Response {
    int status = 0;     // 0 = could not connect
    std::string body;   // JSON text
    std::string error;  // transport error
    bool Ok() const { return status >= 200 && status < 300; }
};

struct HubInfo {
    int port = 0;
    std::string token;
    std::wstring exe;
};

// The running hub's port and token (hub.json). False when the hub is not running.
inline bool ReadHubInfo(HubInfo& info, const std::wstring& home = HubHome()) {
    std::string text, token, exe;
    double port = 0;
    if (!ReadFileUtf8(home + L"\\hub.json", text) || !JsonNumber(text, "port", port) || !JsonString(text, "token", token))
        return false;
    info.port = static_cast<int>(port);
    info.token = token;
    if (JsonString(text, "exe", exe)) info.exe = Widen(exe);
    return info.port > 0;
}

inline Response Request(const HubInfo& hub, const wchar_t* method, const std::string& path, const std::string& body,
                        DWORD timeoutMs = 30000, const std::string& extraHeaders = std::string()) {
    Response r;
    HINTERNET session = WinHttpOpen(L"ModelHubClient/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.error = "WinHttpOpen failed";
        return r;
    }
    WinHttpSetTimeouts(session, 5000, 5000, static_cast<int>(timeoutMs), static_cast<int>(timeoutMs));
    HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(hub.port), 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, method, Widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                         : nullptr;
    if (req) {
        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + Widen(hub.token) + L"\r\n" +
                               Widen(extraHeaders);
        BOOL sent = WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(-1),
                                       body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                       static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
        if (sent && WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            r.status = static_cast<int>(status);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::vector<char> buf(avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, buf.data(), avail, &got) || got == 0) break;
                r.body.append(buf.data(), got);
            }
        } else {
            r.error = "request failed (error " + std::to_string(GetLastError()) + ")";
        }
        WinHttpCloseHandle(req);
    } else {
        r.error = "cannot connect";
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return r;
}

// ---------------------------------------------------------------- client

class Client {
public:
    explicit Client(const std::string& appName, const std::wstring& home = HubHome()) : app_(appName), home_(home) {}

    // Finds the running hub; with startIfNeeded, starts it (ModelHub.exe from install.json, %MODELHUB_EXE%, or PATH)
    // and waits up to waitMs for it to answer.
    bool Connect(bool startIfNeeded = true, DWORD waitMs = 20000, std::string* error = nullptr) {
        if (TryConnect()) return true;
        if (!startIfNeeded) return Fail(error, "ModelHub is not running");
        std::wstring exe = FindExe();
        if (exe.empty()) return Fail(error, "ModelHub is not installed (no install.json, MODELHUB_EXE or ModelHub.exe on PATH)");
        std::wstring cmd = L"\"" + exe + L"\" serve --background";
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(L'\0');
        if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                            nullptr, &si, &pi))
            return Fail(error, "cannot start " + Narrow(exe) + " (error " + std::to_string(GetLastError()) + ")");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        for (DWORD waited = 0; waited < waitMs; waited += 250) {
            if (TryConnect()) return true;
            Sleep(250);
        }
        return Fail(error, "ModelHub did not start within " + std::to_string(waitMs / 1000) + " s");
    }

    bool Connected() const { return info_.port > 0; }
    const std::string& Token() const { return info_.token; }
    int Port() const { return info_.port; }
    std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(info_.port) + "/v1"; }
    const std::string& AppName() const { return app_; }

    Response Get(const std::string& path, DWORD timeoutMs = 30000) const { return Request(info_, L"GET", path, "", timeoutMs, AppHeader()); }
    Response Post(const std::string& path, const std::string& json, DWORD timeoutMs = 30000) const {
        return Request(info_, L"POST", path, json.empty() ? "{}" : json, timeoutMs, AppHeader());
    }
    Response Delete(const std::string& path) const { return Request(info_, L"DELETE", path, "", 30000, AppHeader()); }

    Response Status() const { return Get("/hub/v1/status"); }
    Response Catalog() const { return Get("/hub/v1/catalog"); }
    Response Installed() const { return Get("/hub/v1/models"); }
    // engine: "hub" (served by the hub, GPU when present) or "embedded-cpu" (loaded in-process on the CPU).
    // metricsJson: a JSON object of workload metrics such as {"lines": 4200}, or "" when nothing is known yet.
    Response Advise(const std::string& task, const std::string& engine = "hub", const std::string& metricsJson = "",
                    const std::string& summary = "", const std::string& currentModel = "") const {
        std::string body = "{\"task\":\"" + JsonEscape(task) + "\",\"engine\":\"" + JsonEscape(engine) + "\",\"current\":\"" +
                           JsonEscape(currentModel) + "\"";
        if (!metricsJson.empty())
            body += ",\"workload\":{\"metrics\":" + metricsJson + ",\"summary\":\"" + JsonEscape(summary) + "\"}";
        return Post("/hub/v1/advise", body + "}");
    }
    Response Model(const std::string& id) const { return Get("/hub/v1/models/" + UrlEncode(id) + "?app=" + UrlEncode(app_)); }
    Response StartDownload(const std::string& id) const { return Post("/hub/v1/downloads", "{\"model\":\"" + JsonEscape(id) + "\"}"); }
    Response Downloads() const { return Get("/hub/v1/downloads"); }
    Response CancelDownload(const std::string& id) const { return Delete("/hub/v1/downloads/" + UrlEncode(id)); }
    // Deletes every copy of a model, to the Recycle Bin unless recycle is false. The hub stops its servers of it.
    Response DeleteModel(const std::string& id, bool recycle = true) const {
        return Delete("/hub/v1/models/" + UrlEncode(id) + (recycle ? "" : "?recycle=false"));
    }
    // Stops the hub (it answers first, then shuts down).
    Response Shutdown() const { return Post("/hub/v1/shutdown", "{}"); }
    Response Rescan() const { return Post("/hub/v1/rescan", "{}"); }
    // Shares an application's own model folder: the hub verifies the files there and offers them to every app.
    Response AddFolder(const std::wstring& folder) const {
        return Post("/hub/v1/folders", "{\"path\":\"" + JsonEscape(Narrow(folder)) + "\"}");
    }
    // Loads a model now and waits until it answers (large models take minutes). task: a catalog task id whose
    // context and parallel slots to use, or "" with explicit contextTokens / parallel (0 = hub default).
    Response EnsureLoaded(const std::string& id, const std::string& task = "", int contextTokens = 0, int parallel = 0,
                          DWORD timeoutMs = 15 * 60 * 1000) const {
        return Post("/hub/v1/servers/" + UrlEncode(id) + "/start",
                    "{\"task\":\"" + JsonEscape(task) + "\",\"context_tokens\":" + std::to_string(contextTokens) +
                        ",\"parallel\":" + std::to_string(parallel) + "}",
                    timeoutMs);
    }

    // The installed file of a model (to load in-process), empty when it is not installed. Waits while the hub
    // is still scanning its store (at most waitMs).
    std::wstring ModelPath(const std::string& id, std::string* error = nullptr, DWORD waitMs = 120000) const {
        for (DWORD waited = 0;; waited += 1000) {
            Response r = Model(id);
            if (r.status == 503 && waited < waitMs) {
                Sleep(1000);
                continue;
            }
            std::string path;
            if (r.Ok() && r.body.find("\"installed\":true") != std::string::npos && JsonString(r.body, "path", path))
                return Widen(path);
            if (error) *error = r.status == 0 ? r.error : r.Ok() ? "not installed" : "HTTP " + std::to_string(r.status) + ": " + r.body;
            return std::wstring();
        }
    }

private:
    bool TryConnect() {
        HubInfo info;
        if (!ReadHubInfo(info, home_)) return false;
        Response r = Request(info, L"GET", "/health", "", 2000);
        if (r.status != 200) return false;
        info_ = info;
        return true;
    }
    std::wstring FindExe() const {
        std::string text, exe;
        if (ReadFileUtf8(home_ + L"\\install.json", text) && JsonString(text, "exe", exe) &&
            GetFileAttributesW(Widen(exe).c_str()) != INVALID_FILE_ATTRIBUTES)
            return Widen(exe);
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"MODELHUB_EXE", buf, MAX_PATH * 2);
        if (n > 0 && n < MAX_PATH * 2 && GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) return std::wstring(buf, n);
        n = SearchPathW(nullptr, L"ModelHub.exe", nullptr, MAX_PATH * 2, buf, nullptr);
        return n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
    }
    std::string AppHeader() const { return app_.empty() ? std::string() : "X-ModelHub-App: " + app_ + "\r\n"; }
    static bool Fail(std::string* error, const std::string& msg) {
        if (error) *error = msg;
        return false;
    }

    std::string app_;
    std::wstring home_;
    HubInfo info_;
};

}  // namespace client
}  // namespace modelhub
