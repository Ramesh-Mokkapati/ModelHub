#include "Codex.h"

#include "Util.h"

#include <filesystem>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

namespace fs = std::filesystem;

namespace chat
{

namespace
{

std::wstring Env(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
}

std::wstring CodexHome()
{
    std::wstring home = Env(L"CODEX_HOME");
    return !home.empty() ? home : UserProfileDir() + L"\\.codex";
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

// "data:image/png;base64,...." -> a file Codex can attach; "" when it is not an image.
std::wstring WriteImage(const std::string& url, const std::wstring& dir, int index)
{
    size_t semi = url.find(';'), comma = url.find(',');
    if (url.rfind("data:image/", 0) != 0 || semi == std::string::npos || comma == std::string::npos)
    {
        return {};
    }

    std::string mime = url.substr(5, semi - 5), data = url.substr(comma + 1);
    DWORD size = 0;
    if (!CryptStringToBinaryA(data.c_str(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr,
            nullptr))
    {
        return {};
    }

    std::string bytes(size, '\0');
    if (!CryptStringToBinaryA(data.c_str(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64,
            reinterpret_cast<BYTE*>(bytes.data()), &size, nullptr, nullptr))
    {
        return {};
    }

    std::wstring ext = mime == "image/jpeg" ? L".jpg" : mime == "image/gif" ? L".gif"
                                                    : mime == "image/webp"  ? L".webp"
                                                                            : L".png";
    std::wstring path = dir + L"\\image" + std::to_wstring(index) + ext;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    DWORD written = 0;
    WriteFile(f, bytes.data(), size, &written, nullptr);
    CloseHandle(f);
    return written == size ? path : std::wstring();
}

std::wstring Quote(const std::wstring& s)
{
    return L"\"" + s + L"\"";
}

}  // namespace

std::wstring CodexExe()
{
    std::wstring path = Env(L"CODEX_CLI_PATH");
    if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return path;
    }

    // The Codex app keeps its CLI in a folder per version: the newest one.
    std::error_code ec;
    fs::path best;
    fs::file_time_type bestTime{};
    fs::path root = Env(L"LOCALAPPDATA") + L"\\OpenAI\\Codex\\bin";
    if (fs::is_directory(root, ec))
    {
        for (const auto& dir : fs::directory_iterator(root, ec))
        {
            fs::path exe = dir.path() / L"codex.exe";
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
    }

    if (!best.empty())
    {
        return best.wstring();
    }

    wchar_t exe[MAX_PATH];
    return SearchPathW(nullptr, L"codex.exe", nullptr, MAX_PATH, exe, nullptr) ? std::wstring(exe) : std::wstring();
}

bool CodexSignedIn(std::string* how)
{
    std::string text;
    if (!ReadFileBytes(CodexHome() + L"\\auth.json", text, 1 << 20))
    {
        return false;
    }

    json j = json::parse(text, nullptr, false);
    if (!j.is_object())
    {
        return false;
    }

    bool chatgpt = j.contains("tokens") && j["tokens"].is_object() && !j["tokens"].value("refresh_token", "").empty();
    bool key = j.contains("OPENAI_API_KEY") && j["OPENAI_API_KEY"].is_string() && !j["OPENAI_API_KEY"].get<std::string>().empty();
    if (how)
    {
        *how = chatgpt ? "signed in with ChatGPT" : key ? "signed in with an OpenAI API key"
                                                        : "";
    }

    return chatgpt || key;
}

bool CodexListModels(std::vector<std::string>& models, std::string& error)
{
    models.clear();
    if (CodexExe().empty())
    {
        error = "the Codex CLI was not found (install the Codex app, or: npm install -g @openai/codex)";
        return false;
    }

    if (!CodexSignedIn())
    {
        error = "not signed in (Model Providers > Edit > Sign in, or: codex login)";
        return false;
    }

    std::string text;
    if (ReadFileBytes(CodexHome() + L"\\models_cache.json", text, 16 << 20))
    {
        json j = json::parse(text, nullptr, false);
        if (j.is_object() && j.contains("models") && j["models"].is_array())
        {
            for (const auto& m : j["models"])
            {
                if (m.is_object() && m.value("visibility", "list") == "list" && !m.value("slug", "").empty())
                {
                    models.push_back(m.value("slug", ""));
                }
            }
        }
    }

    if (models.empty())
    {
        error = "Codex has not listed this account's models yet (open the Codex app once, or run: codex)";
    }

    return !models.empty();
}

StreamResult StreamCodex(const Endpoint&, const json& request,
    const std::function<void(DeltaKind, const std::string&)>& onDelta, CancelToken* cancel)
{
    StreamResult res;
    std::wstring exe = CodexExe();
    if (exe.empty())
    {
        res.error = "ChatGPT: the Codex CLI was not found (install the Codex app)";
        return res;
    }

    // One prompt: the instructions, the conversation so far, then the last user message (Codex starts a new thread
    // every time: "--ephemeral" keeps these out of the user's Codex history).
    std::wstring tempDir = fs::temp_directory_path().wstring() + L"\\ModelHubChat-codex-" + std::to_wstring(GetCurrentProcessId()) +
                           L"-" + std::to_wstring(GetTickCount64());
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    std::vector<std::wstring> images;
    std::string system, history, prompt;
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
                    if (part.value("type", "") == "image_url")
                    {
                        std::wstring file = WriteImage(part["image_url"].value("url", ""), tempDir, static_cast<int>(images.size()));
                        if (!file.empty())
                        {
                            images.push_back(file);
                        }
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

    if (history.size() > 120000)
    {
        history = "..." + history.substr(history.size() - 120000);
    }

    std::string input;
    if (!system.empty())
    {
        input += "<instructions>\n" + system + "\n</instructions>\n\n";
    }

    if (!history.empty())
    {
        input += "<conversation_so_far>\n" + history + "</conversation_so_far>\n\n";
    }

    input += prompt;

    std::string model = request.value("model", "");
    std::wstring folder = Wide(request.value("x_workspace", Utf8(UserProfileDir())));
    std::wstring cmd = Quote(exe) + L" exec --json --ephemeral --skip-git-repo-check --color never -s read-only";
    if (!model.empty() && model != "auto")
    {
        cmd += L" -m " + Quote(Wide(model));
    }

    std::string effort = request.value("reasoning_effort", "");
    if (effort == "low" || effort == "medium" || effort == "high")
    {
        cmd += L" -c model_reasoning_effort=" + Wide(effort);
    }

    if (!folder.empty() && GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        cmd += L" -C " + Quote(folder);
    }

    for (const auto& img : images)
    {
        cmd += L" --image=" + Quote(img);
    }

    cmd += L" -- -";  // the prompt comes on stdin

    // Pipes: stdin (the prompt), stdout (JSON lines), stderr (errors). A job ends Codex and its helpers together.
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    CreatePipe(&inRead, &inWrite, &sa, 0);
    CreatePipe(&outRead, &outWrite, &sa, 0);
    CreatePipe(&errRead, &errWrite, &sa, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    PROCESS_INFORMATION pi{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    std::wstring line = cmd;
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
        nullptr, &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    auto cleanup = [&]
    {
        fs::remove_all(tempDir, ec);
    };

    if (!ok)
    {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        CloseHandle(errRead);
        CloseHandle(job);
        cleanup();
        res.error = "ChatGPT: could not start the Codex CLI (" + std::to_string(GetLastError()) + ")";
        return res;
    }

    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // The prompt, from a thread: a long one could fill the pipe before Codex reads it.
    std::thread writer([inWrite, input]
        {
        DWORD n = 0;
        WriteFile(inWrite, input.data(), static_cast<DWORD>(input.size()), &n, nullptr);
        CloseHandle(inWrite); });
    std::string errText;
    std::thread errReader([errRead, &errText]
        {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(errRead, buf, sizeof buf, &n, nullptr) && n > 0)
            if (errText.size() < 64 * 1024)
                errText.append(buf, n); });
    // Cancel: end the job, which closes the pipes and ends the reading below.
    std::atomic<bool> done{ false };
    std::thread watcher([&]
        {
        while (!done) {
            if (cancel && cancel->IsCancelled()) {
                TerminateJobObject(job, 1);
                return;
            }
            Sleep(100);
        } });

    auto started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point firstOutput;
    bool sawOutput = false;
    auto emit = [&](DeltaKind kind, const std::string& text)
    {
        if (text.empty())
        {
            return;
        }

        if (!sawOutput)
        {
            sawOutput = true;
            firstOutput = std::chrono::steady_clock::now();
        }

        (kind == DeltaKind::Reasoning ? res.reasoning : res.content) += text;
        onDelta(kind, text);
    };

    std::string failure, pending;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(outRead, buf, sizeof buf, &n, nullptr) && n > 0)
    {
        pending.append(buf, n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos)
        {
            std::string jsonLine = TrimCopy(pending.substr(0, nl));
            pending.erase(0, nl + 1);
            json e = json::parse(jsonLine, nullptr, false);
            if (!e.is_object())
            {
                continue;
            }

            std::string type = e.value("type", "");
            const json item = e.value("item", json::object());
            std::string itemType = item.value("type", "");
            if (type == "item.completed" && itemType == "agent_message")
            {
                if (!res.content.empty() && res.content.back() != '\n')
                {
                    emit(DeltaKind::Content, "\n\n");
                }

                emit(DeltaKind::Content, item.value("text", ""));
            }
            else if (type == "item.completed" && itemType == "reasoning")
            {
                emit(DeltaKind::Reasoning, item.value("text", "") + "\n");
            }
            else if (type == "item.started" && itemType == "command_execution")
            {
                emit(DeltaKind::Reasoning, "$ " + item.value("command", "") + "\n");
            }
            else if (type == "item.completed" && itemType == "file_change")
            {
                emit(DeltaKind::Reasoning, "(Codex wanted to change files; the read-only sandbox does not allow it)\n");
            }
            else if (type == "item.completed" && itemType == "web_search")
            {
                emit(DeltaKind::Reasoning, "Web search: " + item.value("query", "") + "\n");
            }
            else if (type == "turn.completed")
            {
                const json usage = e.value("usage", json::object());
                res.completionTokens += usage.value("output_tokens", 0);
            }
            else if (type == "turn.failed")
            {
                failure = e.contains("error") && e["error"].is_object() ? e["error"].value("message", "the turn failed")
                                                                        : "the turn failed";
            }
            else if (type == "error")
            {
                failure = e.value("message", "error");
            }
        }
    }

    done = true;
    watcher.join();
    writer.join();
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    errReader.join();
    CloseHandle(outRead);
    CloseHandle(errRead);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    cleanup();

    res.cancelled = cancel && cancel->IsCancelled();
    if (!res.cancelled)
    {
        if (!failure.empty())
        {
            res.error = "ChatGPT: " + failure;
        }
        else if (exitCode != 0 && res.content.empty())
        {
            std::string err = TrimCopy(errText);
            size_t cut = err.rfind('\n');
            res.error = "ChatGPT (Codex exited with " + std::to_string(exitCode) + ")" +
                        (err.empty() ? std::string() : ": " + Truncate(cut == std::string::npos ? err : err.substr(cut + 1), 400, "..."));
        }
    }

    res.servedModel = model;
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - (sawOutput ? firstOutput : started)).count();
    if (sawOutput && secs > 0.2 && res.completionTokens > 0)
    {
        res.tokensPerSecond = res.completionTokens / secs;
    }

    res.finishReason = res.cancelled ? "cancelled" : "stop";
    return res;
}

}  // namespace chat
