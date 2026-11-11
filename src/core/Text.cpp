#include "Text.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace modelhub {

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && static_cast<unsigned char>(s[b]) <= ' ') ++b;
    while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') --e;
    return std::string(s.substr(b, e - b));
}

std::string ToLower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::string ToUpper(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return r;
}

std::string ReplaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool EndsWithI(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    return ToLower(s.substr(s.size() - suffix.size())) == ToLower(suffix);
}

std::vector<std::string> SplitLines(std::string_view s) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            size_t end = i;
            if (end > start && s[end - 1] == '\r') --end;
            if (i < s.size() || end > start) lines.emplace_back(s.substr(start, end - start));
            start = i + 1;
        }
    }
    return lines;
}

std::string ExpandTemplate(std::string_view tmpl, const std::map<std::string, std::string>& vars,
                           std::string_view open, std::string_view close) {
    std::string out;
    out.reserve(tmpl.size());
    size_t i = 0;
    while (i < tmpl.size()) {
        size_t b = tmpl.find(open, i);
        if (b == std::string_view::npos) {
            out.append(tmpl.substr(i));
            break;
        }
        size_t e = tmpl.find(close, b + open.size());
        if (e == std::string_view::npos) {
            out.append(tmpl.substr(i));
            break;
        }
        std::string key(tmpl.substr(b + open.size(), e - b - open.size()));
        out.append(tmpl.substr(i, b - i));
        auto it = vars.find(key);
        if (it != vars.end())
            out.append(it->second);
        else
            out.append(tmpl.substr(b, e + close.size() - b));
        i = e + close.size();
    }
    return out;
}

std::vector<std::wstring> SplitCommandLine(std::wstring_view cmd) {
    std::vector<std::wstring> out;
    std::wstring s(cmd);
    if (Trim(Narrow(s)).empty()) return out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(s.c_str(), &argc);
    if (!argv) return out;
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    LocalFree(argv);
    return out;
}

std::wstring QuoteArg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(arg);
    std::wstring r = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            r.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            r.append(backslashes * 2 + 1, L'\\');
            r.push_back(L'"');
        } else {
            r.append(backslashes, L'\\');
            r.push_back(arg[i]);
        }
    }
    r.push_back(L'"');
    return r;
}

int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string NowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string NewGuid() {
    unsigned char b[16];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("BCryptGenRandom failed");
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

std::string ReadTextFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + PathToUtf8(p));
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    return s;
}

void WriteTextFile(const std::filesystem::path& p, std::string_view content) {
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + PathToUtf8(p));
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void AppendTextFile(const std::filesystem::path& p, std::string_view content) {
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::app);
    if (!f) throw std::runtime_error("cannot append " + PathToUtf8(p));
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string TruncateUtf8(std::string_view s, size_t maxChars, std::string_view marker) {
    if (s.size() <= maxChars) return std::string(s);
    size_t cut = maxChars;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    std::string r(s.substr(0, cut));
    r.append(marker);
    return r;
}

}  // namespace modelhub
