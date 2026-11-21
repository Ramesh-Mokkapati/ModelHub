#include "Util.h"

#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>

#pragma comment(lib, "crypt32.lib")

namespace chat
{

std::wstring Wide(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }

    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Env(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
}

std::wstring ChatHome()
{
    std::wstring home = Env(L"MODELHUB_HOME");
    if (home.empty())
    {
        std::wstring local = Env(L"LOCALAPPDATA");
        home = (local.empty() ? std::wstring(L".") : local) + L"\\ModelHub";
    }

    std::wstring dir = home + L"\\chat";
    SHCreateDirectoryExW(nullptr, (dir + L"\\conversations").c_str(), nullptr);
    return dir;
}

std::wstring UserProfileDir()
{
    std::wstring p = Env(L"USERPROFILE");
    return p.empty() ? L"C:\\" : p;
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

bool ReadFileBytes(const std::wstring& path, std::string& out, size_t maxBytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    out.clear();
    char buf[64 * 1024];
    DWORD got = 0;
    while (out.size() < maxBytes && ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
    {
        out.append(buf, std::min<size_t>(got, maxBytes - out.size()));
    }

    CloseHandle(h);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& bytes)
{
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    BOOL ok = bytes.empty() || WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != bytes.size())
    {
        DeleteFileW(tmp.c_str());
        return false;
    }

    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::string Base64(const std::string& bytes)
{
    if (bytes.empty())
    {
        return {};
    }

    DWORD n = 0;
    const auto* data = reinterpret_cast<const BYTE*>(bytes.data());
    CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string out(n, '\0');
    CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &n);
    out.resize(n);
    return out;
}

std::string FromBase64(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    DWORD n = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr))
    {
        return {};
    }

    std::string out(n, '\0');
    CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
        reinterpret_cast<BYTE*>(out.data()), &n, nullptr, nullptr);
    out.resize(n);
    return out;
}

std::string Protect(const std::string& secret)
{
    if (secret.empty())
    {
        return {};
    }

    DATA_BLOB in{ static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data())) }, out{};
    if (!CryptProtectData(&in, L"ModelHubChat", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
    {
        return {};
    }

    std::string blob(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return Base64(blob);
}

std::string Unprotect(const std::string& protectedText)
{
    std::string blob = FromBase64(protectedText);
    if (blob.empty())
    {
        return {};
    }

    DATA_BLOB in{ static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data()) }, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
    {
        return {};
    }

    std::string secret(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return secret;
}

std::string RunCapture(const std::wstring& commandLine, DWORD timeoutMs)
{
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        return {};
    }

    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = nullptr;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(commandLine.begin(), commandLine.end());
    line.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok)
    {
        CloseHandle(readPipe);
        return {};
    }

    std::string out;
    ULONGLONG start = GetTickCount64();
    for (;;)
    {
        DWORD avail = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
        {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(readPipe, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0)
            {
                break;
            }

            out.append(buf, got);
        }

        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0)
        {
            break;
        }

        if (GetTickCount64() - start > timeoutMs)
        {
            TerminateProcess(pi.hProcess, 1);
            out.clear();
            break;
        }
    }

    DWORD avail = 0;
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
    {
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(readPipe, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0)
        {
            break;
        }

        out.append(buf, got);
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);
    return code == 0 ? out : std::string();
}

static std::wstring GhExe()
{
    wchar_t exe[MAX_PATH];
    return SearchPathW(nullptr, L"gh.exe", nullptr, MAX_PATH, exe, nullptr) ? std::wstring(exe) : std::wstring();
}

static bool SafeHost(const std::string& host)
{
    for (char c : host)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':'))
        {
            return false;  // it goes on a command line
        }
    }

    return true;
}

std::string GhToken(const std::string& hostIn)
{
    std::string host = Lower(TrimCopy(hostIn));
    if (host.empty())
    {
        host = "github.com";
    }

    if (!SafeHost(host))
    {
        return {};
    }

    static std::mutex mu;
    static std::map<std::string, std::pair<std::string, ULONGLONG>> cache;  // host -> token, when fetched
    std::lock_guard lk(mu);
    auto it = cache.find(host);

    // A token is kept 10 minutes; "not signed in" only 10 seconds, so a fresh "gh auth login" counts at once.
    ULONGLONG keep = it != cache.end() && !it->second.first.empty() ? 10 * 60 * 1000 : 10 * 1000;
    if (it != cache.end() && GetTickCount64() - it->second.second < keep)
    {
        return it->second.first;
    }

    std::string token;
    std::wstring exe = GhExe();
    if (!exe.empty())
    {
        token = TrimCopy(RunCapture(L"\"" + exe + L"\" auth token --hostname " + Wide(host), 10000));
    }

    cache[host] = { token, GetTickCount64() };
    return token;
}

std::vector<std::string> GhHosts()
{
    std::vector<std::string> hosts;
    std::wstring exe = GhExe();
    if (exe.empty())
    {
        return hosts;
    }

    // "gh auth status" prints "Logged in to <host> account <name> (...)" per host and account.
    std::string text = RunCapture(L"cmd.exe /d /c \"\"" + exe + L"\" auth status 2>&1\"", 15000);
    size_t pos = 0;
    const std::string key = "Logged in to ";
    while ((pos = text.find(key, pos)) != std::string::npos)
    {
        pos += key.size();
        size_t end = text.find_first_of(" \r\n", pos);
        std::string host = Lower(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        if (!host.empty() && SafeHost(host) && std::find(hosts.begin(), hosts.end(), host) == hosts.end())
        {
            hosts.push_back(host);
        }
    }

    return hosts;
}

std::vector<std::string> GitHubHostsFromCredentials()
{
    // Entry names only ("cmdkey /list" never shows the secrets), e.g.
    //   LegacyGeneric:target=git:https://user@company.ghe.com
    //   LegacyGeneric:target=GitHub for Visual Studio - https://user@company.ghe.com/
    //   LegacyGeneric:target=gh:github.com:user
    std::vector<std::string> hosts;
    std::string text = RunCapture(L"cmdkey.exe /list", 10000);
    size_t pos = 0;
    while ((pos = text.find("target=", pos)) != std::string::npos)
    {
        pos += 7;
        size_t eol = text.find_first_of("\r\n", pos);
        std::string target = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        std::string host;
        size_t scheme = target.find("https://");
        if (scheme != std::string::npos)
        {
            host = target.substr(scheme + 8);
            size_t at = host.find('@'), slash = host.find('/');
            if (at != std::string::npos && (slash == std::string::npos || at < slash))
            {
                host = host.substr(at + 1);
            }

            host = host.substr(0, host.find_first_of("/:"));  // "https://host:user:github.copilot-cli" -> host
        }
        else if (target.rfind("gh:", 0) == 0)
        {
            host = target.substr(3, target.find(':', 3) - 3);
        }

        host = Lower(TrimCopy(host));
        bool github = host.size() > 8 && (host.compare(host.size() - 8, 8, ".ghe.com") == 0 ||
                                             host.find("github") != std::string::npos);
        if (github && host != "api.github.com" && SafeHost(host) &&
            std::find(hosts.begin(), hosts.end(), host) == hosts.end())
        {
            hosts.push_back(host);
        }
    }
    return hosts;
}

std::string AntToken()
{
    static std::mutex mu;
    static std::string token;
    static ULONGLONG fetched = 0;
    std::lock_guard lk(mu);

    ULONGLONG keep = token.empty() ? 10 * 1000 : 5 * 60 * 1000;  // short-lived tokens: fetch again often
    if (fetched && GetTickCount64() - fetched < keep)
    {
        return token;
    }

    token.clear();
    std::wstring exe = AntExe();
    if (!exe.empty())
    {
        token = TrimCopy(RunCapture(L"\"" + exe + L"\" auth print-credentials --access-token", 15000));
    }

    fetched = GetTickCount64();
    return token;
}

std::wstring AntToolsDir()
{
    std::wstring chat = ChatHome();  // <ModelHub home>\chat
    return chat.substr(0, chat.find_last_of(L'\\')) + L"\\tools\\ant";
}

std::wstring AntExe()
{
    wchar_t exe[MAX_PATH];
    if (SearchPathW(nullptr, L"ant.exe", nullptr, MAX_PATH, exe, nullptr))
    {
        return exe;
    }

    std::wstring ours = AntToolsDir() + L"\\ant.exe";
    return GetFileAttributesW(ours.c_str()) != INVALID_FILE_ATTRIBUTES ? ours : std::wstring();
}

bool IsValidUtf8(const std::string& s)
{
    return s.empty() ||
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}

std::string ToUtf8FromCodePage(const std::string& bytes, UINT codePage)
{
    if (bytes.empty())
    {
        return {};
    }

    int n = MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
    return Utf8(w);
}

long long NowEpoch()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string NewId()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    static std::mt19937 rng{ std::random_device{}() };
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04u%02u%02u-%02u%02u%02u-%04x", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
        t.wSecond, static_cast<unsigned>(rng() & 0xffff));
    return buf;
}

std::string LocalDateTime()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    TIME_ZONE_INFORMATION tz;
    DWORD mode = GetTimeZoneInformation(&tz);
    long bias = -(tz.Bias + (mode == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias));
    static const char* days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    char buf[160];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02u%c%02ld:%02ld (%s, %s)", t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, bias < 0 ? '-' : '+', std::labs(bias) / 60, std::labs(bias) % 60,
        days[t.wDayOfWeek % 7], Utf8(mode == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightName : tz.StandardName).c_str());
    return buf;
}

std::string TrimCopy(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
    {
        return {};
    }

    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
        { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Truncate(const std::string& s, size_t maxBytes, const char* note)
{
    if (s.size() <= maxBytes)
    {
        return s;
    }

    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)  // not inside a UTF-8 sequence
    {
        --cut;
    }

    return s.substr(0, cut) + note;
}

}  // namespace chat
