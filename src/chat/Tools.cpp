#include "Tools.h"

#include "Util.h"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace chat
{

namespace
{

constexpr size_t kMaxOutput = 40000;  // bytes of tool output sent back to the model

json Schema(json properties, std::vector<std::string> required)
{
    return { { "type", "object" }, { "properties", std::move(properties) }, { "required", std::move(required) } };
}

json Def(const char* name, const char* description, json parameters)
{
    return { { "type", "function" },
        { "function", { { "name", name }, { "description", description }, { "parameters", std::move(parameters) } } } };
}

const std::map<std::string, json>& Definitions()
{
    static const std::map<std::string, json> defs = {
        { "read_file",
            Def("read_file",
                "Read a text file. Returns its lines numbered from 1. Relative paths are in the workspace folder.",
                Schema({ { "path", { { "type", "string" }, { "description", "File path" } } },
                           { "offset", { { "type", "integer" }, { "description", "First line to read (1-based)" } } },
                           { "limit", { { "type", "integer" }, { "description", "Number of lines (default 2000)" } } } },
                    { "path" })) },
        { "file_glob_search",
            Def("file_glob_search",
                "Find files by glob pattern, e.g. **/*.cpp or src/*.h. A pattern without '/' matches file names in every "
                "folder. Returns paths relative to the searched folder.",
                Schema({ { "pattern", { { "type", "string" }, { "description", "Glob pattern" } } },
                           { "path", { { "type", "string" }, { "description", "Folder to search (default: workspace)" } } } },
                    { "pattern" })) },
        { "grep_search",
            Def("grep_search",
                "Search file contents with a regular expression (ECMAScript syntax). Returns path:line: text for each "
                "match.",
                Schema({ { "pattern", { { "type", "string" }, { "description", "Regular expression" } } },
                           { "path", { { "type", "string" }, { "description", "File or folder (default: workspace)" } } },
                           { "include", { { "type", "string" }, { "description", "Only files matching this glob, e.g. *.cpp" } } },
                           { "case_insensitive", { { "type", "boolean" } } },
                           { "max_results", { { "type", "integer" }, { "description", "Default 100" } } } },
                    { "pattern" })) },
        { "exec_shell_command",
            Def("exec_shell_command",
                "Run a shell command in the workspace folder on Windows and return its exit code and output (stdout "
                "and stderr).",
                Schema({ { "command", { { "type", "string" }, { "description", "The command line" } } },
                           { "timeout", { { "type", "integer" }, { "description", "Seconds (default 60, at most 600)" } } } },
                    { "command" })) },
        { "write_file",
            Def("write_file", "Create or overwrite a file with the given content. Creates missing folders.",
                Schema({ { "path", { { "type", "string" } } }, { "content", { { "type", "string" } } } },
                    { "path", "content" })) },
        { "edit_file",
            Def("edit_file",
                "Replace an exact piece of text in a file. old_string must occur exactly once unless replace_all is "
                "true.",
                Schema({ { "path", { { "type", "string" } } }, { "old_string", { { "type", "string" } } },
                           { "new_string", { { "type", "string" } } }, { "replace_all", { { "type", "boolean" } } } },
                    { "path", "old_string", "new_string" })) },
        { "get_datetime",
            Def("get_datetime", "Get the current local date, time, weekday and time zone.", Schema(json::object(), {})) },
    };
    return defs;
}

std::string Str(const json& args, const char* key, const std::string& def = {})
{
    if (!args.is_object() || !args.contains(key))
    {
        return def;
    }

    const json& v = args[key];
    return v.is_string() ? v.get<std::string>() : v.is_null() ? def
                                                              : v.dump();
}

long long Int(const json& args, const char* key, long long def)
{
    if (!args.is_object() || !args.contains(key))
    {
        return def;
    }

    const json& v = args[key];
    if (v.is_number())
    {
        return v.get<long long>();
    }

    if (v.is_string())
    {
        try
        {
            return std::stoll(v.get<std::string>());
        }
        catch (...)
        {
        }
    }

    return def;
}

bool Bool(const json& args, const char* key)
{
    if (!args.is_object() || !args.contains(key))
    {
        return false;
    }

    const json& v = args[key];
    return v.is_boolean() ? v.get<bool>() : v.is_string() && Lower(v.get<std::string>()) == "true";
}

fs::path Resolve(const ToolContext& ctx, const std::string& p)
{
    fs::path path(Wide(p.empty() ? std::string(".") : p));
    if (path.is_relative())
    {
        path = fs::path(ctx.workspace) / path;
    }

    return path.lexically_normal();
}

std::string Display(const fs::path& p)
{
    return Utf8(p.wstring());
}

std::string RelSlash(const fs::path& p, const fs::path& base)
{
    std::wstring s = p.lexically_relative(base).wstring();
    std::replace(s.begin(), s.end(), L'\\', L'/');
    return Utf8(s);
}

bool SkipDir(const fs::path& dir)
{
    static const std::set<std::wstring> skip = { L".git", L".svn", L".hg", L".vs", L"node_modules", L"__pycache__" };
    return skip.count(dir.filename().wstring()) > 0;
}

std::wregex GlobRegex(const std::string& glob)
{
    std::wstring g = Wide(glob), re;
    int braces = 0;
    for (size_t i = 0; i < g.size(); ++i)
    {
        wchar_t c = g[i];
        if (c == L'*')
        {
            if (i + 1 < g.size() && g[i + 1] == L'*')
            {
                ++i;
                if (i + 1 < g.size() && (g[i + 1] == L'/' || g[i + 1] == L'\\'))
                {
                    ++i;
                    re += L"(.*/)?";
                }
                else
                {
                    re += L".*";
                }
            }
            else
            {
                re += L"[^/]*";
            }
        }
        else if (c == L'?')
        {
            re += L"[^/]";
        }
        else if (c == L'\\' || c == L'/')
        {
            re += L"/";
        }
        else if (c == L'{')
        {
            ++braces;
            re += L"(";
        }
        else if (c == L'}' && braces > 0)
        {
            --braces;
            re += L")";
        }
        else if (c == L',' && braces > 0)
        {
            re += L"|";
        }
        else if (std::wstring(L".+()^$|[]").find(c) != std::wstring::npos)
        {
            re += std::wstring(L"\\") + c;
        }
        else
        {
            re += c;
        }
    }

    return std::wregex(re, std::regex::icase | std::regex::ECMAScript);
}

// Walks files under root (skipping .git and similar); fn returns false to stop.
void WalkFiles(const fs::path& root, CancelToken* cancel, const std::function<bool(const fs::path&)>& fn)
{
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec))
    {
        if (cancel && cancel->IsCancelled())
        {
            return;
        }

        const auto& entry = *it;
        std::error_code ec2;
        if (entry.is_directory(ec2))
        {
            if (SkipDir(entry.path()))
            {
                it.disable_recursion_pending();
            }

            continue;
        }

        if (entry.is_regular_file(ec2) && !fn(entry.path()))
        {
            return;
        }
    }
}

std::string ReadFileTool(const json& args, const ToolContext& ctx)
{
    fs::path path = Resolve(ctx, Str(args, "path"));
    std::string bytes;
    std::error_code ec;
    if (fs::is_directory(path, ec))
    {
        return "Error: " + Display(path) + " is a folder; use file_glob_search to list files";
    }

    if (!ReadFileBytes(path.wstring(), bytes, 32 * 1024 * 1024))
    {
        return "Error: cannot read " + Display(path) + " (not found or no access)";
    }

    if (bytes.find('\0') != std::string::npos && bytes.find('\0') < 8192)
    {
        return "Error: " + Display(path) + " is a binary file (" + std::to_string(bytes.size()) + " bytes)";
    }

    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0)
    {
        bytes.erase(0, 3);
    }

    if (!IsValidUtf8(bytes))
    {
        bytes = ToUtf8FromCodePage(bytes, CP_ACP);
    }

    long long offset = std::max(1LL, Int(args, "offset", 1)), limit = std::max(1LL, Int(args, "limit", 2000));
    std::ostringstream out;
    long long line = 0;
    size_t pos = 0;
    bool more = false;
    while (pos <= bytes.size())
    {
        size_t eol = bytes.find('\n', pos);
        if (eol == std::string::npos)
        {
            eol = bytes.size();
        }

        ++line;
        if (line >= offset)
        {
            if (line >= offset + limit || static_cast<size_t>(out.tellp()) > kMaxOutput)
            {
                more = true;
                break;
            }

            std::string text = bytes.substr(pos, eol - pos);
            if (!text.empty() && text.back() == '\r')
            {
                text.pop_back();
            }

            char num[16];
            std::snprintf(num, sizeof num, "%6lld\t", line);
            out << num << text << '\n';
        }

        if (eol == bytes.size())
        {
            break;
        }

        pos = eol + 1;
    }

    std::string s = out.str();
    if (s.empty())
    {
        return "(file has " + std::to_string(line) + " lines; nothing at offset " + std::to_string(offset) + ")";
    }

    if (more)
    {
        s += "... (more lines: read again with offset " + std::to_string(line) + ")\n";
    }

    return s;
}

std::string GlobTool(const json& args, const ToolContext& ctx)
{
    std::string pattern = Str(args, "pattern");
    if (pattern.empty())
    {
        return "Error: pattern is required";
    }

    fs::path root = Resolve(ctx, Str(args, "path"));
    std::error_code ec;
    if (!fs::is_directory(root, ec))
    {
        return "Error: " + Display(root) + " is not a folder";
    }

    bool nameOnly = pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos;
    std::wregex re;
    try
    {
        re = GlobRegex(pattern);
    }
    catch (const std::regex_error&)
    {
        return "Error: invalid glob pattern";
    }

    std::vector<std::string> found;
    size_t total = 0;
    WalkFiles(root, ctx.cancel,
        [&](const fs::path& p)
        {
            std::wstring subject = nameOnly ? p.filename().wstring() : Wide(RelSlash(p, root));
            if (std::regex_match(subject, re))
            {
                ++total;
                if (found.size() < 500)
                {
                    found.push_back(RelSlash(p, root));
                }
            }
            return true;
        });

    if (found.empty())
    {
        return "No files match " + pattern + " in " + Display(root);
    }

    std::sort(found.begin(), found.end());
    std::string out;
    for (const auto& f : found)
    {
        out += f + "\n";
    }

    if (total > found.size())
    {
        out += "... and " + std::to_string(total - found.size()) + " more\n";
    }

    return out;
}

std::string GrepTool(const json& args, const ToolContext& ctx)
{
    std::string pattern = Str(args, "pattern");
    if (pattern.empty())
    {
        return "Error: pattern is required";
    }

    auto flags = std::regex::ECMAScript;
    if (Bool(args, "case_insensitive"))
    {
        flags |= std::regex::icase;
    }

    std::regex re;
    try
    {
        re = std::regex(pattern, flags);
    }
    catch (const std::regex_error& e)
    {
        return std::string("Error: invalid regular expression: ") + e.what();
    }

    std::string include = Str(args, "include");
    std::wregex includeRe;
    if (!include.empty())
    {
        try
        {
            includeRe = GlobRegex(include);
        }
        catch (const std::regex_error&)
        {
            return "Error: invalid include glob";
        }
    }

    size_t maxResults = static_cast<size_t>(std::clamp(Int(args, "max_results", 100), 1LL, 1000LL));
    fs::path root = Resolve(ctx, Str(args, "path"));
    std::error_code ec;
    bool single = fs::is_regular_file(root, ec);
    if (!single && !fs::is_directory(root, ec))
    {
        return "Error: " + Display(root) + " does not exist";
    }

    fs::path base = single ? root.parent_path() : root;
    std::string out;
    size_t matches = 0, files = 0;
    auto searchFile = [&](const fs::path& p) -> bool
    {
        if (!include.empty() && !std::regex_match(p.filename().wstring(), includeRe) &&
            !std::regex_match(Wide(RelSlash(p, base)), includeRe))
        {
            return true;
        }

        std::error_code e2;
        if (fs::file_size(p, e2) > 4 * 1024 * 1024)
        {
            return true;
        }

        std::string bytes;
        if (!ReadFileBytes(p.wstring(), bytes))
        {
            return true;
        }

        if (bytes.find('\0') != std::string::npos)
        {
            return true;  // binary
        }

        ++files;
        size_t pos = 0, line = 0;
        while (pos < bytes.size())
        {
            size_t eol = bytes.find('\n', pos);
            if (eol == std::string::npos)
            {
                eol = bytes.size();
            }

            ++line;
            std::string text = bytes.substr(pos, std::min<size_t>(eol - pos, 2000));
            if (!text.empty() && text.back() == '\r')
            {
                text.pop_back();
            }

            if (std::regex_search(text, re))
            {
                if (++matches > maxResults)
                {
                    return false;
                }

                out += RelSlash(p, base) + ":" + std::to_string(line) + ": " + Truncate(TrimCopy(text), 300, "...") + "\n";
            }

            pos = eol + 1;
        }

        return true;
    };

    try
    {
        if (single)
        {
            searchFile(root);
        }
        else
        {
            WalkFiles(root, ctx.cancel, searchFile);
        }
    }
    catch (const std::exception& e)
    {
        return std::string("Error: ") + e.what();
    }

    if (out.empty())
    {
        return "No matches for " + pattern + " (" + std::to_string(files) + " files searched)";
    }

    if (matches > maxResults)
    {
        out += "... (stopped at " + std::to_string(maxResults) + " matches)\n";
    }

    return out;
}

std::string ShellTool(const json& args, const ToolContext& ctx)
{
    std::string command = Str(args, "command");
    if (TrimCopy(command).empty())
    {
        return "Error: command is required";
    }

    DWORD timeoutMs = static_cast<DWORD>(std::clamp(Int(args, "timeout", 60), 1LL, 600LL) * 1000);
    if (ctx.confirmShell && ctx.confirm &&
        !ctx.confirm("Run this command?", "Folder: " + Utf8(ctx.workspace) + "\n\n" + command))
    {
        return "Error: the user declined to run this command";
    }

    std::wstring cmdline;
    if (ctx.shell == "powershell")
    {
        std::wstring w = Wide(command);
        std::string utf16(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
        cmdline = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + Wide(Base64(utf16));
    }
    else
    {
        cmdline = L"cmd.exe /d /s /c \"" + Wide(command) + L"\"";
    }

    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        return "Error: CreatePipe failed";
    }

    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job)
    {
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }

    std::vector<wchar_t> line(cmdline.begin(), cmdline.end());
    line.push_back(L'\0');
    std::wstring dir = ctx.workspace;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
    {
        dir = UserProfileDir();
    }

    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
        dir.c_str(), &si, &pi);
    DWORD startError = GetLastError();
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE)
    {
        CloseHandle(nul);
    }

    if (!ok)
    {
        CloseHandle(readPipe);
        if (job)
        {
            CloseHandle(job);
        }

        return "Error: cannot start the shell (error " + std::to_string(startError) + ")";
    }

    if (job)
    {
        AssignProcessToJobObject(job, pi.hProcess);
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::string output;
    bool timedOut = false, cancelled = false;
    ULONGLONG start = GetTickCount64();
    auto drain = [&]
    {
        DWORD avail = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
        {
            char buf[8192];
            DWORD got = 0;
            if (!ReadFile(readPipe, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0)
            {
                break;
            }

            output.append(buf, got);
            if (output.size() > 4 * kMaxOutput)  // keep the end
            {
                output.erase(0, output.size() - 2 * kMaxOutput);
            }
        }
    };

    for (;;)
    {
        DWORD w = WaitForSingleObject(pi.hProcess, 50);
        drain();
        if (w == WAIT_OBJECT_0)
        {
            break;
        }

        if (ctx.cancel && ctx.cancel->IsCancelled())
        {
            cancelled = true;
            break;
        }

        if (GetTickCount64() - start > timeoutMs)
        {
            timedOut = true;
            break;
        }
    }

    if (timedOut || cancelled)
    {
        if (job)
        {
            TerminateJobObject(job, 1);
        }
        else
        {
            TerminateProcess(pi.hProcess, 1);
        }

        WaitForSingleObject(pi.hProcess, 2000);
    }

    drain();
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    if (job)
    {
        CloseHandle(job);
    }

    if (!IsValidUtf8(output))
    {
        output = ToUtf8FromCodePage(output, CP_OEMCP);
    }

    if (output.size() > kMaxOutput)
    {
        output = "... (start of output dropped)\n" + output.substr(output.size() - kMaxOutput);
    }

    std::string head = timedOut    ? "Timed out after " + std::to_string(timeoutMs / 1000) + " s (stopped)\n"
                       : cancelled ? "Cancelled\n"
                                   : "Exit code: " + std::to_string(static_cast<long>(code)) + "\n";
    return head + (output.empty() ? "(no output)" : output);
}

std::string WriteTool(const json& args, const ToolContext& ctx)
{
    std::string p = Str(args, "path");
    if (p.empty())
    {
        return "Error: path is required";
    }

    fs::path path = Resolve(ctx, p);
    std::string content = Str(args, "content");
    std::error_code ec;
    bool exists = fs::exists(path, ec);
    if (ctx.confirmWrites && ctx.confirm &&
        !ctx.confirm(exists ? "Overwrite this file?" : "Create this file?",
            Display(path) + " (" + std::to_string(content.size()) + " bytes)\n\n" + Truncate(content, 1500)))
    {
        return "Error: the user declined to write " + Display(path);
    }

    fs::create_directories(path.parent_path(), ec);
    if (!WriteFileAtomic(path.wstring(), content))
    {
        return "Error: cannot write " + Display(path);
    }

    return std::string(exists ? "Overwrote " : "Created ") + Display(path) + " (" + std::to_string(content.size()) + " bytes)";
}

std::string EditTool(const json& args, const ToolContext& ctx)
{
    fs::path path = Resolve(ctx, Str(args, "path"));
    std::string oldS = Str(args, "old_string"), newS = Str(args, "new_string");
    if (oldS.empty())
    {
        return "Error: old_string is required";
    }

    std::string bytes;
    if (!ReadFileBytes(path.wstring(), bytes))
    {
        return "Error: cannot read " + Display(path);
    }

    // Files with CRLF endings: match the model's LF text too.
    bool crlf = bytes.find("\r\n") != std::string::npos;
    auto toCrlf = [](const std::string& s)
    {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
            {
                o += '\r';
            }

            o += s[i];
        }

        return o;
    };

    if (crlf && bytes.find(oldS) == std::string::npos)
    {
        oldS = toCrlf(oldS);
        newS = toCrlf(newS);
    }

    size_t count = 0;
    for (size_t at = bytes.find(oldS); at != std::string::npos; at = bytes.find(oldS, at + oldS.size()))
    {
        ++count;
    }

    if (count == 0)
    {
        return "Error: old_string was not found in " + Display(path) + " (it must match exactly, including spaces)";
    }

    bool all = Bool(args, "replace_all");
    if (count > 1 && !all)
    {
        return "Error: old_string occurs " + std::to_string(count) +
               " times; give more surrounding text so it is unique, or set replace_all";
    }

    if (ctx.confirmWrites && ctx.confirm &&
        !ctx.confirm("Edit this file?",
            Display(path) + "\n\nReplace" + (all ? " (all " + std::to_string(count) + ")" : std::string()) + ":\n" +
                Truncate(oldS, 700) + "\n\nWith:\n" + Truncate(newS, 700)))
    {
        return "Error: the user declined the edit";
    }

    std::string out;
    size_t pos = 0;
    for (size_t at = bytes.find(oldS); at != std::string::npos; at = bytes.find(oldS, pos))
    {
        out.append(bytes, pos, at - pos);
        out += newS;
        pos = at + oldS.size();
        if (!all)
        {
            break;
        }
    }

    out.append(bytes, pos, std::string::npos);
    if (!WriteFileAtomic(path.wstring(), out))
    {
        return "Error: cannot write " + Display(path);
    }

    return "Edited " + Display(path) + " (" + std::to_string(all ? count : 1) + " replacement" +
           (all && count > 1 ? "s" : "") + ")";
}

}  // namespace

const std::vector<std::string>& BuiltinToolNames()
{
    static const std::vector<std::string> names = { "read_file", "file_glob_search", "grep_search", "exec_shell_command",
        "write_file", "edit_file", "get_datetime" };
    return names;
}

bool IsBuiltinTool(const std::string& name)
{
    return Definitions().count(name) > 0;
}

json BuiltinToolDefinition(const std::string& name)
{
    auto it = Definitions().find(name);
    return it == Definitions().end() ? json() : it->second;
}

std::string RunBuiltinTool(const std::string& name, const json& args, const ToolContext& ctx)
{
    try
    {
        if (name == "read_file")
        {
            return ReadFileTool(args, ctx);
        }

        if (name == "file_glob_search")
        {
            return GlobTool(args, ctx);
        }

        if (name == "grep_search")
        {
            return GrepTool(args, ctx);
        }

        if (name == "exec_shell_command")
        {
            return ShellTool(args, ctx);
        }

        if (name == "write_file")
        {
            return WriteTool(args, ctx);
        }

        if (name == "edit_file")
        {
            return EditTool(args, ctx);
        }

        if (name == "get_datetime")
        {
            return LocalDateTime();
        }
    }
    catch (const std::exception& e)
    {
        return std::string("Error: ") + e.what();
    }

    return "Error: unknown tool " + name;
}

}  // namespace chat
