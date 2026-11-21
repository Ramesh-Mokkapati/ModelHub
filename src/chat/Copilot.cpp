#include "Copilot.h"

#include "Util.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <random>

namespace fs = std::filesystem;

namespace chat
{

namespace
{

std::string NewUuid()
{
    static std::mt19937_64 rng{ std::random_device{}() };
    static std::mutex mu;
    std::lock_guard lk(mu);
    char buf[40];
    unsigned long long a = rng(), b = rng();
    std::snprintf(buf, sizeof buf, "%08llx-%04llx-4%03llx-%04llx-%012llx", a >> 32, (a >> 16) & 0xffff, a & 0xfff,
        ((b >> 48) & 0x3fff) | 0x8000, b & 0xffffffffffffULL);
    return buf;
}

// "https://company.ghe.com/" -> "company.ghe.com"
std::string HostName(const std::string& host)
{
    std::string h = Lower(TrimCopy(host));
    size_t scheme = h.find("://");
    if (scheme != std::string::npos)
    {
        h.erase(0, scheme + 3);
    }

    h = h.substr(0, h.find('/'));
    return h.empty() ? std::string("github.com") : h;
}

std::wstring NewestUnder(const std::wstring& root, const wchar_t* name)
{
    std::error_code ec;
    fs::path best;
    fs::file_time_type bestTime{};
    if (!fs::is_directory(root, ec))
    {
        return {};
    }

    for (const auto& dir : fs::directory_iterator(root, ec))
    {
        fs::path exe = dir.path() / name;
        if (!fs::is_regular_file(exe, ec))
        {
            continue;
        }

        auto t = fs::last_write_time(exe, ec);
        if (best.empty() || t > bestTime)
        {
            best = exe;
            bestTime = t;
        }
    }

    return best.wstring();
}

// One Copilot runtime process for one GitHub host.
class Runtime
{
public:
    explicit Runtime(std::string host)
        : host_(std::move(host))
    {
    }

    ~Runtime()
    {
        Stop();
    }

    bool Start(std::string& error)
    {
        std::wstring exe = CopilotRuntimeExe();
        if (exe.empty())
        {
            error = "the GitHub Copilot runtime is not on this PC (install the GitHub Copilot app or the Copilot CLI)";
            return false;
        }

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

        // This process's environment, with COPILOT_GH_HOST naming the account's host (github.com or *.ghe.com).
        std::wstring env;
        if (wchar_t* block = GetEnvironmentStringsW())
        {
            for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
            {
                if (_wcsnicmp(p, L"COPILOT_GH_HOST=", 16) != 0)
                {
                    env.append(p).push_back(L'\0');
                }
            }

            FreeEnvironmentStringsW(block);
        }

        env += L"COPILOT_GH_HOST=" + Wide(HostName(host_));
        env.push_back(L'\0');
        env.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = nul;

        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe + L"\" --headless --no-auto-update --stdio";
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(L'\0');

        BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env.data(), UserProfileDir().c_str(), &si, &pi);
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
            error = "cannot start the Copilot runtime (error " + std::to_string(err) + ")";
            return false;
        }

        job_ = CreateJobObjectW(nullptr, nullptr);  // the runtime and its children end with this application
        if (job_)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            AssignProcessToJobObject(job_, pi.hProcess);
        }

        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        stdin_ = inWrite;
        stdout_ = outRead;
        done_ = false;
        reader_ = std::thread([this]
            { ReadLoop(); });

        json r = Call("connect", { { "supportedTaskKinds", json::array({ "agent", "client", "shell" }) } }, 60000, error);

        if (r.is_null() && error.find("-32601") != std::string::npos)
        {
            error.clear();
            r = Call("ping", { { "message", "ModelHubChat" } }, 30000, error);
        }

        return !r.is_null();
    }

    bool Alive()
    {
        std::lock_guard lk(mu_);
        return !done_;
    }

    // A request; null with an error ("Copilot: ... (code n)") when it fails.
    json Call(const std::string& method, const json& params, DWORD timeoutMs, std::string& error, CancelToken* cancel = nullptr)
    {
        std::string id = NewUuid();
        {
            std::lock_guard lk(mu_);
            if (done_)
            {
                error = "the Copilot runtime stopped";
                return json();
            }

            waiting_.insert(id);
        }

        Write({ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } });
        std::unique_lock lk(mu_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!replies_.count(id))
        {
            if (done_ || (cancel && cancel->IsCancelled()) || std::chrono::steady_clock::now() > deadline)
            {
                waiting_.erase(id);
                error = done_ ? "the Copilot runtime stopped" : cancel && cancel->IsCancelled() ? "cancelled"
                                                                                                : method + " timed out";
                return json();
            }

            cv_.wait_for(lk, std::chrono::milliseconds(100));
        }

        json reply = std::move(replies_[id]);
        replies_.erase(id);
        waiting_.erase(id);
        if (reply.contains("error"))
        {
            const json& e = reply["error"];
            error = "Copilot: " + e.value("message", e.dump()) + " (code " + std::to_string(e.value("code", 0)) + ")";
            return json();
        }

        return reply.contains("result") ? reply["result"] : json::object();
    }

    void Listen(const std::string& sessionId, std::function<void(const json&)> fn)
    {
        std::lock_guard lk(mu_);
        listeners_[sessionId] = std::move(fn);
    }

    void Unlisten(const std::string& sessionId)
    {
        std::lock_guard lk(mu_);
        listeners_.erase(sessionId);
    }

private:
    void Write(const json& msg)
    {
        std::string body = msg.dump(-1, ' ', false, json::error_handler_t::replace);
        std::string data = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        std::lock_guard lk(writeMu_);
        DWORD written = 0;
        WriteFile(stdin_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    }

    void ReadLoop()
    {
        std::string buf;
        char chunk[65536];
        for (;;)
        {
            DWORD got = 0;
            if (!ReadFile(stdout_, chunk, sizeof chunk, &got, nullptr) || got == 0)
            {
                break;
            }

            buf.append(chunk, got);

            for (;;)
            {
                size_t end = buf.find("\r\n\r\n");
                if (end == std::string::npos)
                {
                    break;
                }

                std::string header = Lower(buf.substr(0, end));
                size_t at = header.find("content-length:");
                size_t length = at == std::string::npos ? 0 : std::strtoull(header.c_str() + at + 15, nullptr, 10);
                if (buf.size() < end + 4 + length)
                {
                    break;
                }

                json msg = json::parse(buf.substr(end + 4, length), nullptr, false);
                buf.erase(0, end + 4 + length);
                if (msg.is_object())
                {
                    Dispatch(msg);
                }
            }
        }

        std::lock_guard lk(mu_);
        done_ = true;
        cv_.notify_all();
    }

    void Dispatch(const json& msg)
    {
        bool hasId = msg.contains("id") && !msg["id"].is_null();
        if (hasId && !msg.contains("method"))
        {
            std::string id = msg["id"].is_string() ? msg["id"].get<std::string>() : msg["id"].dump();
            std::lock_guard lk(mu_);
            if (waiting_.count(id))
            {
                replies_[id] = msg;
                cv_.notify_all();
            }

            return;
        }

        std::string method = msg.value("method", "");
        if (hasId)
        {
            // The runtime asks the client (user input, shell authentication, ...): not offered by this client.
            Write({ { "jsonrpc", "2.0" }, { "id", msg["id"] },
                { "error", { { "code", -32601 }, { "message", "Method not found: " + method } } } });
            return;
        }

        if (method == "session.event" && msg.contains("params"))
        {
            const json& p = msg["params"];
            std::function<void(const json&)> fn;
            {
                std::lock_guard lk(mu_);
                auto it = listeners_.find(p.value("sessionId", ""));
                if (it != listeners_.end())
                {
                    fn = it->second;
                }
            }

            if (fn && p.contains("event"))
            {
                fn(p["event"]);
            }
        }
    }

    void Stop()
    {
        if (job_)
        {
            TerminateJobObject(job_, 0);
        }
        else if (process_)
        {
            TerminateProcess(process_, 0);
        }

        if (reader_.joinable())
        {
            reader_.join();
        }

        for (HANDLE h : { process_, stdin_, stdout_, job_ })
        {
            if (h)
            {
                CloseHandle(h);
            }
        }
    }

    std::string host_;
    HANDLE process_ = nullptr, stdin_ = nullptr, stdout_ = nullptr, job_ = nullptr;
    std::thread reader_;
    std::mutex mu_, writeMu_;
    std::condition_variable cv_;
    bool done_ = true;
    std::set<std::string> waiting_;
    std::map<std::string, json> replies_;
    std::map<std::string, std::function<void(const json&)>> listeners_;
};

std::shared_ptr<Runtime> GetRuntime(const std::string& host, std::string& error)
{
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<Runtime>> runtimes;  // by host name; they live as long as the app
    std::lock_guard lk(mu);
    std::string key = HostName(host);

    auto it = runtimes.find(key);
    if (it != runtimes.end() && it->second->Alive())
    {
        return it->second;
    }

    auto rt = std::make_shared<Runtime>(key);
    if (!rt->Start(error))
    {
        return nullptr;
    }

    runtimes[key] = rt;
    return rt;
}

std::string TextOf(const json& content)
{
    if (content.is_string())
    {
        return content.get<std::string>();
    }

    std::string out;
    if (content.is_array())
    {
        for (const auto& part : content)
        {
            if (part.is_object() && part.value("type", "") == "text")
            {
                out += part.value("text", "");
            }
        }
    }

    return out;
}

}  // namespace

std::wstring CopilotRuntimeExe()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"COPILOT_CLI_PATH", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2 && GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES)
    {
        return std::wstring(buf, n);
    }

    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    std::wstring root = std::wstring(buf, n) + L"\\github-copilot-sdk\\cli";
    return NewestUnder(root, L"copilot-runtime.exe");
}

std::wstring CopilotCliExe()
{
    std::wstring runtime = CopilotRuntimeExe();
    if (!runtime.empty())
    {
        std::wstring cli = runtime.substr(0, runtime.find_last_of(L'\\')) + L"\\copilot.exe";
        if (GetFileAttributesW(cli.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            return cli;
        }
    }

    wchar_t exe[MAX_PATH];
    return SearchPathW(nullptr, L"copilot.exe", nullptr, MAX_PATH, exe, nullptr) ? std::wstring(exe) : std::wstring();
}

CopilotAuth CopilotAuthStatus(const std::string& host)
{
    CopilotAuth a;
    a.runtimeFound = !CopilotRuntimeExe().empty();
    if (!a.runtimeFound)
    {
        a.error = "the GitHub Copilot runtime is not on this PC";
        return a;
    }

    auto rt = GetRuntime(host, a.error);
    if (!rt)
    {
        return a;
    }

    json s = rt->Call("auth.getStatus", json::object(), 30000, a.error);
    if (s.is_object())
    {
        a.signedIn = s.value("isAuthenticated", false);
        if (s.contains("login") && s["login"].is_string())
        {
            a.login = s["login"].get<std::string>();
        }

        // The runtime falls back to any stored Copilot sign-in: it only counts when it is for this host.
        std::string signedHost = s.contains("host") && s["host"].is_string() ? HostName(s["host"].get<std::string>()) : "";
        if (a.signedIn && !signedHost.empty() && signedHost != HostName(host))
        {
            a.signedIn = false;
            a.error = "Copilot is signed in to " + signedHost + ", not " + HostName(host) +
                      " (Model Providers > Edit > Sign in, to add this account)";
        }
    }

    return a;
}

bool CopilotListModels(const std::string& host, std::vector<std::string>& models, std::string& error)
{
    models.clear();
    CopilotAuth a = CopilotAuthStatus(host);
    if (!a.signedIn)
    {
        error = !a.error.empty() ? a.error
                                 : "not signed in to " + HostName(host) + " (Model Providers > Edit > Sign in, or: copilot login" +
                                       (HostName(host) == "github.com" ? std::string() : " --host https://" + HostName(host)) + ")";
        return false;
    }

    auto rt = GetRuntime(host, error);
    if (!rt)
    {
        return false;
    }

    json r = rt->Call("models.list", json::object(), 60000, error);
    if (r.is_object() && r.contains("models"))
    {
        for (const auto& m : r["models"])
        {
            if (m.is_object() && !m.value("id", "").empty())
            {
                models.push_back(m.value("id", ""));
            }
        }
    }

    if (models.empty() && error.empty())
    {
        error = "this Copilot account offers no models (is Copilot enabled for it?)";
    }

    return !models.empty();
}

StreamResult StreamCopilot(const Endpoint& ep, const json& request,
    const std::function<void(DeltaKind, const std::string&)>& onDelta, CancelToken* cancel, const ToolRunner& runTool)
{
    StreamResult res;
    auto rt = GetRuntime(ep.baseUrl, res.error);
    if (!rt)
    {
        return res;
    }

    // The conversation so far goes in as context; the last user message is the prompt.
    std::string system, history, prompt;
    json attachments = json::array();
    const json messages = request.value("messages", json::array());
    size_t last = messages.size();
    for (size_t i = messages.size(); i-- > 0;)
    {
        if (messages[i].value("role", "") == "user")
        {
            last = i;
            break;
        }
    }

    for (size_t i = 0; i < messages.size(); ++i)
    {
        const json& m = messages[i];
        std::string role = m.value("role", "");
        std::string text = TextOf(m.value("content", json("")));
        if (role == "system")
        {
            system += (system.empty() ? "" : "\n\n") + text;
        }
        else if (i == last)
        {
            prompt = text;
            if (m.contains("content") && m["content"].is_array())
            {
                for (const auto& part : m["content"])
                {
                    std::string url = part.value("type", "") == "image_url" ? part["image_url"].value("url", "") : "";
                    size_t semi = url.find(';'), comma = url.find(',');
                    if (url.rfind("data:", 0) == 0 && semi != std::string::npos && comma != std::string::npos)
                    {
                        attachments.push_back({ { "type", "blob" }, { "data", url.substr(comma + 1) },
                            { "mimeType", url.substr(5, semi - 5) }, { "displayName", "image" } });
                    }
                }
            }
        }
        else if (role == "user")
        {
            history += "User: " + text + "\n\n";
        }
        else if (role == "assistant" && !TrimCopy(text).empty())
        {
            history += "Assistant: " + text + "\n\n";
        }
        else if (role == "tool")
        {
            history += "(Tool result: " + Truncate(text, 2000, "...") + ")\n\n";
        }
    }

    if (!history.empty())
    {
        if (history.size() > 120000)
        {
            history = "..." + history.substr(history.size() - 120000);
        }

        prompt = "<conversation_so_far>\n" + history + "</conversation_so_far>\n\n" + prompt;
    }

    // Only the app's tools: Copilot's own shell, file and MCP tools stay off.
    json defs = json::array(), names = json::array();
    for (const auto& t : request.value("tools", json::array()))
    {
        const json& f = t["function"];
        defs.push_back({ { "name", f.value("name", "") }, { "description", f.value("description", "") },
            { "parameters", f.contains("parameters") ? f["parameters"] : json{ { "type", "object" } } }, { "hasHandler", true },
            { "skipPermission", true } });
        names.push_back(f.value("name", ""));
    }

    std::string sessionId = NewUuid();
    json create = { { "sessionId", sessionId }, { "model", request.value("model", "auto") }, { "streaming", true },
        { "tools", defs }, { "availableTools", names }, { "requestPermission", true }, { "enableConfigDiscovery", false },
        { "workingDirectory", request.value("x_workspace", Utf8(UserProfileDir())) } };

    if (!system.empty())
    {
        create["systemMessage"] = { { "mode", "append" }, { "content", system } };
    }

    std::string effort = request.value("reasoning_effort", "");
    if (effort == "low" || effort == "medium" || effort == "high")
    {
        create["reasoningEffort"] = effort;
    }

    // What the event callbacks share: a callback can still be running just after Unlisten, so it keeps this alive.
    struct State
    {
        std::mutex mu;
        std::condition_variable cv;
        bool idle = false;
        std::set<std::string> streamed;  // messages whose text came as deltas
        std::vector<std::future<void>> work;
        StreamResult res;
        bool sawOutput = false;
        std::chrono::steady_clock::time_point firstOutput;
    };

    auto st = std::make_shared<State>();
    auto started = std::chrono::steady_clock::now();
    std::function<void(DeltaKind, const std::string&)> deliver = onDelta;
    ToolRunner tools = runTool;
    rt->Listen(sessionId, [st, deliver, tools, rt, sessionId](const json& e)
        {
        std::string type = e.value("type", "");
        const json d = e.value("data", json::object());
        std::lock_guard lk(st->mu);
        auto emit = [&](DeltaKind kind, const std::string& text)
        {
            if (text.empty())
            {
                return;
            }

            if (!st->sawOutput)
            {
                st->sawOutput = true;
                st->firstOutput = std::chrono::steady_clock::now();
            }
            
            (kind == DeltaKind::Reasoning ? st->res.reasoning : st->res.content) += text;
            deliver(kind, text);
        };

        if (type == "assistant.message_delta")
        {
            st->streamed.insert(d.value("messageId", ""));
            emit(DeltaKind::Content, d.value("deltaContent", ""));
        } else if (type == "assistant.reasoning_delta")
        {
            emit(DeltaKind::Reasoning, d.value("deltaContent", ""));
        } else if (type == "assistant.message")
        {
            if (!st->streamed.count(d.value("messageId", "")))
            {
                emit(DeltaKind::Content, d.value("content", ""));
            }
            
            if (!st->res.content.empty() && st->res.content.back() != '\n' && !d.value("toolRequests", json::array()).empty())
            {
                emit(DeltaKind::Content, "\n\n");
            }
            
            if (d.contains("model") && d["model"].is_string())
            {
                st->res.servedModel = d["model"].get<std::string>();
            }
        }
        else if (type == "assistant.usage")
        {
            st->res.completionTokens += d.value("outputTokens", 0);
        }
        else if (type == "external_tool.requested")
        {
            // The app's tool, on a worker: the reader thread must stay free for the reply.
            std::string requestId = d.value("requestId", ""), name = d.value("toolName", "");
            json args = d.value("arguments", json::object());
            st->work.push_back(std::async(std::launch::async, [rt, sessionId, requestId, name, args, tools] {
                std::string result = tools ? tools(name, args.is_object() ? args : json::object()) : "Error: no tools";
                std::string ignored;
                rt->Call("session.tools.handlePendingToolCall",
                    { { "sessionId", sessionId }, { "requestId", requestId }, { "result", result } }, 60000, ignored);
            }));
        }
        else if (type == "permission.requested")
        {
            // Only the app's tools are available; anything else Copilot asks for is refused.
            std::string requestId = d.value("requestId", "");
            st->work.push_back(std::async(std::launch::async, [rt, sessionId, requestId] {
                std::string ignored;
                rt->Call("session.permissions.handlePendingPermissionRequest",
                    { { "sessionId", sessionId }, { "requestId", requestId },
                        { "result", { { "kind", "denied-interactively-by-user" } } } }, 30000, ignored);
            }));
        }
        else if (type == "session.error")
        {
            st->res.error = "Copilot: " + d.value("message", "error");
            st->idle = true;
            st->cv.notify_all();
        }
        else if (type == "session.idle")
        {
            st->idle = true;
            st->cv.notify_all();
        } });

    std::string error;
    json created = rt->Call("session.create", create, 120000, error, cancel);
    if (created.is_null())
    {
        rt->Unlisten(sessionId);
        res.cancelled = cancel && cancel->IsCancelled();
        res.error = res.cancelled ? std::string() : error;
        return res;
    }

    json send = { { "sessionId", sessionId }, { "prompt", prompt } };
    if (!attachments.empty())
    {
        send["attachments"] = attachments;
    }

    bool cancelled = false;
    std::string failure;
    if (rt->Call("session.send", send, 120000, error, cancel).is_null())
    {
        failure = error;
    }
    else
    {
        std::unique_lock lk(st->mu);
        auto deadline = started + std::chrono::minutes(30);
        while (!st->idle)
        {
            if (!rt->Alive())
            {
                failure = "the Copilot runtime stopped";
                break;
            }

            if (cancel && cancel->IsCancelled())
            {
                lk.unlock();
                std::string ignored;
                rt->Call("session.abort", { { "sessionId", sessionId } }, 10000, ignored);
                cancelled = true;
                lk.lock();
                st->cv.wait_for(lk, std::chrono::seconds(5), [&]
                    { return st->idle; });
                break;
            }

            if (std::chrono::steady_clock::now() > deadline)
            {
                failure = "Copilot did not finish within 30 minutes";
                break;
            }

            st->cv.wait_for(lk, std::chrono::milliseconds(200));
        }
    }

    rt->Unlisten(sessionId);
    std::vector<std::future<void>> pending;
    {
        std::lock_guard lk(st->mu);
        pending.swap(st->work);
        res = st->res;
    }

    for (auto& w : pending)
    {
        w.wait();
    }

    std::string ignored;
    rt->Call("session.delete", { { "sessionId", sessionId } }, 30000, ignored);
    if (!failure.empty())
    {
        res.error = failure;
    }

    res.cancelled = cancelled;
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - st->firstOutput).count();
    if (st->sawOutput && secs > 0.2 && res.completionTokens > 0)
    {
        res.tokensPerSecond = res.completionTokens / secs;
    }

    res.finishReason = res.cancelled ? "cancelled" : "stop";
    return res;
}

}  // namespace chat
