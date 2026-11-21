#include "Mcp.h"

#include "AppMessages.h"
#include "Util.h"

#include <chrono>

namespace chat
{

namespace
{

constexpr const char* kProtocolVersion = "2025-06-18";

// A server's processes (cmd.exe, npx, node, ...) are in one job: they end together, and with the application.
HANDLE NewKillJob()
{
    HANDLE j = CreateJobObjectW(nullptr, nullptr);
    if (j)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }

    return j;
}

std::string ContentText(const json& content)
{
    std::string out;
    if (!content.is_array())
    {
        return content.is_string() ? content.get<std::string>() : content.dump();
    }

    for (const auto& item : content)
    {
        if (!item.is_object())
        {
            continue;
        }

        std::string type = item.value("type", "");
        std::string piece;
        if (type == "text")
        {
            piece = item.value("text", "");
        }
        else if (type == "image" || type == "audio")
        {
            piece = "[" + type + " " + item.value("mimeType", "") + "]";
        }
        else if (type == "resource" && item.contains("resource") && item["resource"].is_object())
        {
            piece = item["resource"].value("text", "[resource " + item["resource"].value("uri", "") + "]");
        }
        else if (type == "resource_link")
        {
            piece = "[resource " + item.value("uri", "") + "]";
        }
        else
        {
            piece = item.dump();
        }

        if (!out.empty())
        {
            out += "\n";
        }

        out += piece;
    }

    return out;
}

std::string RpcError(const json& e)
{
    if (e.is_object())
    {
        return e.value("message", e.dump()) + (e.contains("code") ? " (code " + e["code"].dump() + ")" : "");
    }

    return e.dump();
}

}  // namespace

McpServer::McpServer(McpServerConfig config)
    : config_(std::move(config))
{
}

McpServer::~McpServer()
{
    Disconnect();
}

void McpServer::SetState(const std::string& s)
{
    std::lock_guard lk(dataMu_);
    state_ = s;
}

McpServer::Info McpServer::Snapshot() const
{
    std::lock_guard lk(dataMu_);
    return { config_, state_, tools_, resources_ };
}

bool McpServer::StartProcess(std::string& error)
{
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0))
    {
        error = "CreatePipe failed";
        return false;
    }

    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = nul;  // servers log to stderr; nobody reads it
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"cmd.exe /d /s /c \"" + Wide(config_.command) + L"\"";
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    std::wstring dir = UserProfileDir();
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
        dir.c_str(), &si, &pi);
    DWORD err = GetLastError();
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (nul != INVALID_HANDLE_VALUE)
    {
        CloseHandle(nul);
    }

    if (!ok)
    {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        error = "cannot start the command (error " + std::to_string(err) + ")";
        return false;
    }

    HANDLE job = NewKillJob();
    if (job)
    {
        AssignProcessToJobObject(job, pi.hProcess);
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard lk(procMu_);
        job_ = job;
        process_ = pi.hProcess;
        stdinWrite_ = inWrite;
        stdoutRead_ = outRead;
    }

    {
        std::lock_guard lk(rpcMu_);
        readerDone_ = false;
        responses_.clear();
    }

    reader_ = std::thread([this]
        { ReaderLoop(); });
    return true;
}

void McpServer::ReaderLoop()
{
    std::string buf;
    char chunk[16384];
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(stdoutRead_, chunk, sizeof chunk, &got, nullptr) || got == 0)
        {
            break;
        }

        buf.append(chunk, got);
        size_t start = 0;
        for (size_t nl; (nl = buf.find('\n', start)) != std::string::npos; start = nl + 1)
        {
            std::string line = TrimCopy(buf.substr(start, nl - start));
            if (line.empty() || (line[0] != '{' && line[0] != '['))
            {
                continue;  // not JSON-RPC (some servers print banners)
            }

            json msg = json::parse(line, nullptr, false);
            if (msg.is_array())
            {
                for (const auto& m : msg)
                {
                    Dispatch(m);
                }
            }
            else if (msg.is_object())
            {
                Dispatch(msg);
            }
        }

        buf.erase(0, start);
    }

    std::lock_guard lk(rpcMu_);
    readerDone_ = true;
    rpcCv_.notify_all();
}

void McpServer::Dispatch(const json& msg)
{
    if (!msg.is_object())
    {
        return;
    }

    bool isResponse = msg.contains("id") && (msg.contains("result") || msg.contains("error")) && !msg.contains("method");
    if (isResponse && msg["id"].is_number_integer())
    {
        std::lock_guard lk(rpcMu_);
        responses_[msg["id"].get<long long>()] = msg;
        rpcCv_.notify_all();
        return;
    }

    if (msg.contains("method") && msg.contains("id"))  // a request from the server
    {
        std::string method = msg.value("method", "");
        json reply = { { "jsonrpc", "2.0" }, { "id", msg["id"] } };
        if (method == "ping")
        {
            reply["result"] = json::object();
        }
        else if (method == "roots/list")
        {
            reply["result"] = { { "roots", json::array() } };
        }
        else
        {
            reply["error"] = { { "code", -32601 }, { "message", "not supported by ModelHubChat" } };
        }

        WriteLine(reply.dump());
    }
}

bool McpServer::WriteLine(const std::string& line)
{
    std::lock_guard lk(writeMu_);
    HANDLE h;
    {
        std::lock_guard pl(procMu_);
        h = stdinWrite_;
    }

    if (!h)
    {
        return false;
    }

    std::string data = line + "\n";
    DWORD written = 0;
    return WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
}

json McpServer::HttpRpc(const json& msg, DWORD timeoutMs, std::string& error, CancelToken* cancel)
{
    HttpHeaders headers = { { "Content-Type", "application/json" }, { "Accept", "application/json, text/event-stream" } };
    if (!config_.token.empty())
    {
        headers.push_back({ "Authorization", "Bearer " + config_.token });
    }

    if (!sessionId_.empty())
    {
        headers.push_back({ "Mcp-Session-Id", sessionId_ });
    }

    if (!protocolVersion_.empty())
    {
        headers.push_back({ "MCP-Protocol-Version", protocolVersion_ });
    }

    HttpResult r = HttpSend(L"POST", config_.url, msg.dump(), headers, timeoutMs, {}, cancel ? cancel : &connectCancel_);
    std::string sid = r.Header("mcp-session-id");
    if (!sid.empty())
    {
        sessionId_ = sid;
    }

    if (!msg.contains("id"))
    {
        return json::object();  // notification: 202 Accepted
    }

    if (!r.Ok())
    {
        if (r.cancelled)
        {
            error = "cancelled";
        }
        else if (r.status == 0)
        {
            error = r.error;
        }
        else
        {
            error = "HTTP " + std::to_string(r.status) + (r.status == 401 ? " (check the token)" : "") + ": " +
                    Truncate(r.body, 300);
        }

        return json();
    }

    long long id = msg["id"].get<long long>();
    auto matches = [id](const json& m)
    { return m.is_object() && m.contains("id") && m["id"] == id; };

    if (r.Header("content-type").find("text/event-stream") != std::string::npos)
    {
        json found;
        SseParser sse;
        auto onEvent = [&](const std::string&, const std::string& data)
        {
            json m = json::parse(data, nullptr, false);
            if (matches(m))
            {
                found = m;
                return false;
            }

            return true;
        };

        if (sse.Feed(r.body.data(), r.body.size(), onEvent))
        {
            sse.Finish(onEvent);
        }

        if (found.is_null())
        {
            error = "no response in the event stream";
        }

        return found;
    }

    json m = json::parse(r.body, nullptr, false);
    if (m.is_array())
    {
        for (const auto& item : m)
        {
            if (matches(item))
            {
                return item;
            }
        }
    }

    if (matches(m))
    {
        return m;
    }

    error = "unexpected response: " + Truncate(r.body, 300);
    return json();
}

json McpServer::Request(const std::string& method, const json& params, DWORD timeoutMs, std::string& error,
    CancelToken* cancel)
{
    long long id = nextId_++;
    json msg = { { "jsonrpc", "2.0" }, { "id", id }, { "method", method } };
    if (!params.is_null())
    {
        msg["params"] = params;
    }

    json reply;
    if (config_.transport == "http")
    {
        reply = HttpRpc(msg, timeoutMs, error, cancel);
        if (reply.is_null())
        {
            return json();
        }
    }
    else
    {
        if (!WriteLine(msg.dump()))
        {
            error = "the server is not running";
            return json();
        }

        std::unique_lock lk(rpcMu_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            auto it = responses_.find(id);
            if (it != responses_.end())
            {
                reply = std::move(it->second);
                responses_.erase(it);
                break;
            }

            if (readerDone_)
            {
                error = "the server stopped";
                return json();
            }

            if ((cancel && cancel->IsCancelled()) || connectCancel_.IsCancelled())
            {
                error = "cancelled";
                return json();
            }

            if (std::chrono::steady_clock::now() > deadline)
            {
                error = method + " timed out";
                return json();
            }

            rpcCv_.wait_for(lk, std::chrono::milliseconds(100));
        }
    }

    if (reply.contains("error"))
    {
        error = RpcError(reply["error"]);
        return json();
    }

    return reply.contains("result") ? reply["result"] : json::object();
}

void McpServer::Notify(const std::string& method, const json& params)
{
    json msg = { { "jsonrpc", "2.0" }, { "method", method } };
    if (!params.is_null())
    {
        msg["params"] = params;
    }

    if (config_.transport == "http")
    {
        std::string ignored;
        HttpRpc(msg, 15000, ignored, nullptr);
    }
    else
    {
        WriteLine(msg.dump());
    }
}

bool McpServer::Connect()
{
    std::lock_guard life(lifeMu_);
    SetState("connecting");
    std::string error;
    auto fail = [&](const std::string& why)
    {
        SetState("error: " + why);
        return false;
    };

    if (config_.transport == "http")
    {
        if (TrimCopy(config_.url).empty())
        {
            return fail("no URL");
        }
    }
    else
    {
        if (TrimCopy(config_.command).empty())
        {
            return fail("no command");
        }

        if (!StartProcess(error))
        {
            return fail(error);
        }
    }

    json init = Request("initialize",
        { { "protocolVersion", kProtocolVersion }, { "capabilities", json::object() },
            { "clientInfo", { { "name", "ModelHubChat" }, { "version", "1.0" } } } },
        120000, error);

    if (init.is_null())
    {
        return fail(error);
    }

    protocolVersion_ = init.value("protocolVersion", kProtocolVersion);
    Notify("notifications/initialized", json());

    std::vector<McpTool> tools;
    std::string cursor;
    for (int page = 0; page < 20; ++page)
    {
        json params = cursor.empty() ? json::object() : json{ { "cursor", cursor } };
        json r = Request("tools/list", params, 60000, error);
        if (r.is_null())
        {
            break;
        }

        if (r.contains("tools") && r["tools"].is_array())
        {
            for (const auto& t : r["tools"])
            {
                if (!t.is_object() || !t.contains("name"))
                {
                    continue;
                }

                McpTool tool;
                tool.name = t.value("name", "");
                tool.description = t.value("description", "");
                tool.inputSchema = t.contains("inputSchema") && t["inputSchema"].is_object()
                                       ? t["inputSchema"]
                                       : json{ { "type", "object" }, { "properties", json::object() } };
                tools.push_back(tool);
            }
        }

        cursor = r.value("nextCursor", "");
        if (cursor.empty())
        {
            break;
        }
    }

    std::vector<McpResource> resources;
    bool hasResources = init.contains("capabilities") && init["capabilities"].is_object() &&
                        init["capabilities"].contains("resources");
    if (hasResources)
    {
        std::string ignored;
        json r = Request("resources/list", json::object(), 30000, ignored);
        if (r.is_object() && r.contains("resources") && r["resources"].is_array())
        {
            for (const auto& x : r["resources"])
            {
                if (x.is_object() && x.contains("uri"))
                {
                    resources.push_back({ x.value("uri", ""), x.value("name", x.value("uri", "")),
                        x.value("description", ""), x.value("mimeType", "") });
                }
            }
        }
    }
    {
        std::lock_guard lk(dataMu_);
        tools_ = std::move(tools);
        resources_ = std::move(resources);
        state_ = "connected";
    }
    return true;
}

void McpServer::Disconnect()
{
    connectCancel_.Cancel();
    {
        std::lock_guard lk(procMu_);
        if (job_)
        {
            TerminateJobObject(job_, 0);
        }
        else if (process_)
        {
            TerminateProcess(process_, 0);
        }
    }

    std::lock_guard life(lifeMu_);
    {
        std::lock_guard lk(procMu_);  // again: Connect may have started it after the first try
        if (job_)
        {
            TerminateJobObject(job_, 0);
        }
        else if (process_)
        {
            TerminateProcess(process_, 0);
        }
    }

    if (reader_.joinable())
    {
        reader_.join();
    }
    {
        std::lock_guard lk(procMu_);
        for (HANDLE* h : { &process_, &stdinWrite_, &stdoutRead_, &job_ })
        {
            if (*h)
            {
                CloseHandle(*h);
                *h = nullptr;
            }
        }
    }

    std::lock_guard lk(dataMu_);
    state_ = "off";
    tools_.clear();
    resources_.clear();
}

std::string McpServer::CallTool(const std::string& tool, const json& args, CancelToken* cancel, bool& isError)
{
    std::string error;
    json r = Request("tools/call", { { "name", tool }, { "arguments", args.is_object() ? args : json::object() } },
        10 * 60 * 1000, error, cancel);

    if (r.is_null())
    {
        isError = true;
        return "Error: " + error;
    }

    isError = r.value("isError", false);
    std::string text = r.contains("content") ? ContentText(r["content"]) : std::string();
    if (text.empty() && r.contains("structuredContent"))
    {
        text = r["structuredContent"].dump(2);
    }

    return text.empty() ? "(no output)" : text;
}

std::string McpServer::ReadResource(const std::string& uri, std::string& error)
{
    json r = Request("resources/read", { { "uri", uri } }, 60000, error);
    if (r.is_null())
    {
        return {};
    }

    std::string out;
    if (r.contains("contents") && r["contents"].is_array())
    {
        for (const auto& c : r["contents"])
        {
            if (!c.is_object())
            {
                continue;
            }

            if (!out.empty())
            {
                out += "\n";
            }

            out += c.contains("text") ? c.value("text", "") : "[binary " + c.value("mimeType", "") + "]";
        }
    }

    return out;
}

// ---------------------------------------------------------------- McpManager

McpManager::~McpManager()
{
    Shutdown();
}

std::string McpManager::QualifiedName(const std::string& server, const std::string& tool)
{
    std::string n = server + "__" + tool;
    for (char& c : n)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
        {
            c = '_';
        }
    }

    if (n.size() > 64)
    {
        n.resize(64);
    }

    return n;
}

void McpManager::ConnectAsync(const std::shared_ptr<McpServer>& s, HWND notify)
{
    auto work = [s, notify]()
    {
        ::PostMessage(notify, WM_APP_MCP_CHANGED, 0, 0);
        s->Connect();
        ::PostMessage(notify, WM_APP_MCP_CHANGED, 0, 0);
    };
    std::thread(work).detach();
}

void McpManager::Apply(const std::vector<McpServerConfig>& configs, HWND notify)
{
    std::vector<std::shared_ptr<McpServer>> next, dropped;
    {
        std::lock_guard lk(mu_);
        std::vector<std::shared_ptr<McpServer>> old = servers_;
        for (const auto& cfg : configs)
        {
            std::shared_ptr<McpServer> keep;
            for (auto& s : old)
            {
                if (s && s->Config().SameConnection(cfg))
                {
                    keep = s;
                    s.reset();
                    break;
                }
            }

            if (!keep)
            {
                keep = std::make_shared<McpServer>(cfg);
                if (cfg.enabled)
                {
                    ConnectAsync(keep, notify);
                }
            }

            next.push_back(keep);
        }

        for (auto& s : old)
        {
            if (s)
            {
                dropped.push_back(s);
            }
        }

        servers_ = next;
    }

    for (auto& s : dropped)
    {
        std::thread([s]
            { s->Disconnect(); })
            .detach();
    }

    ::PostMessage(notify, WM_APP_MCP_CHANGED, 0, 0);
}

void McpManager::Reconnect(HWND notify)
{
    std::vector<McpServerConfig> configs;
    {
        std::lock_guard lk(mu_);
        for (auto& s : servers_)
        {
            configs.push_back(s->Config());
        }

        std::vector<std::shared_ptr<McpServer>> old;
        old.swap(servers_);
        for (auto& s : old)
        {
            std::thread([s]
                { s->Disconnect(); })
                .detach();
        }
    }

    Apply(configs, notify);
}

void McpManager::Shutdown()
{
    std::vector<std::shared_ptr<McpServer>> old;
    {
        std::lock_guard lk(mu_);
        old.swap(servers_);
    }

    for (auto& s : old)
    {
        s->Disconnect();  // quick: it cancels what runs and ends the process
    }
}

std::vector<std::shared_ptr<McpServer>> McpManager::Servers() const
{
    std::lock_guard lk(mu_);
    return servers_;
}

}  // namespace chat
