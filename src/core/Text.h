#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace modelhub {

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);
inline std::string PathToUtf8(const std::filesystem::path& p) { return Narrow(p.wstring()); }
inline std::filesystem::path Utf8ToPath(std::string_view s) { return std::filesystem::path(Widen(s)); }

std::string Trim(std::string_view s);
std::string ToLower(std::string_view s);
std::string ToUpper(std::string_view s);
std::string ReplaceAll(std::string s, std::string_view from, std::string_view to);
bool EndsWithI(std::string_view s, std::string_view suffix);
std::vector<std::string> SplitLines(std::string_view s);

// Replaces {key} placeholders. Unknown placeholders are left as is.
std::string ExpandTemplate(std::string_view tmpl, const std::map<std::string, std::string>& vars,
                           std::string_view open = "{", std::string_view close = "}");

// Splits a command line the way Windows programs parse argv (CommandLineToArgvW rules).
std::vector<std::wstring> SplitCommandLine(std::wstring_view cmd);

// Quotes one argument so that CommandLineToArgvW / the MSVC CRT round-trips it.
std::wstring QuoteArg(std::wstring_view arg);

int64_t NowUnix();
std::string NowIso8601();
std::string NewGuid();

std::string ReadTextFile(const std::filesystem::path& p);  // throws std::runtime_error
void WriteTextFile(const std::filesystem::path& p, std::string_view content);
void AppendTextFile(const std::filesystem::path& p, std::string_view content);

// Keeps at most maxChars characters, appending a marker when cut. Never splits a UTF-8 sequence.
std::string TruncateUtf8(std::string_view s, size_t maxChars, std::string_view marker = "\n...[truncated]");

}  // namespace modelhub
