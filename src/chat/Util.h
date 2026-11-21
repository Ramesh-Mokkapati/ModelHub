#pragma once
#include "stdafx.h"

namespace chat
{

std::wstring Wide(const std::string& utf8);
std::string Utf8(const std::wstring& w);
inline CString W(const std::string& utf8)
{
    return CString(Wide(utf8).c_str());
}
inline std::string U8(const CString& s)
{
    return Utf8(std::wstring(s.GetString()));
}

// %MODELHUB_HOME%\chat, else %LOCALAPPDATA%\ModelHub\chat (created on demand).
std::wstring ChatHome();
std::wstring UserProfileDir();
std::wstring ExeDir();

bool ReadFileBytes(const std::wstring& path, std::string& out, size_t maxBytes = static_cast<size_t>(-1));
bool WriteFileAtomic(const std::wstring& path, const std::string& bytes);

std::string Base64(const std::string& bytes);
std::string FromBase64(const std::string& text);
// DPAPI: only this Windows user on this PC can read it back. Empty in, empty out.
std::string Protect(const std::string& secret);
std::string Unprotect(const std::string& protectedText);
bool IsValidUtf8(const std::string& s);
// Runs a program hidden and returns what it wrote (stdout), or "" when it fails or takes longer than timeoutMs.
std::string RunCapture(const std::wstring& commandLine, DWORD timeoutMs = 10000);
// The GitHub CLI's token (gh auth token [--hostname host]), cached for 10 minutes per host; "" when gh is missing
// or not signed in there. host: "" or github.com, or a GitHub Enterprise host (company.ghe.com, github.company.com).
std::string GhToken(const std::string& host = "");
// The hosts the GitHub CLI is signed in to (github.com and GitHub Enterprise hosts).
std::vector<std::string> GhHosts();
// GitHub and GitHub Enterprise hosts named by Windows Credential Manager entries (Git Credential Manager, Visual
// Studio, the GitHub CLI): what the GitHub Copilot app and other GitHub tools are signed in to. Names only.
std::vector<std::string> GitHubHostsFromCredentials();
// Anthropic's CLI sign-in (ant auth print-credentials --access-token), cached briefly; "" without it.
std::string AntToken();
// Anthropic's CLI: on PATH, or the copy ModelHub Chat installed in <ModelHub home>\tools\ant; "" when neither.
std::wstring AntExe();
std::wstring AntToolsDir();
std::string ToUtf8FromCodePage(const std::string& bytes, UINT codePage);

long long NowEpoch();
std::string NewId();
std::string LocalDateTime();  // 2026-11-20 15:30:00 (Friday), with the UTC offset
std::string TrimCopy(const std::string& s);
std::string Lower(std::string s);
std::string Truncate(const std::string& s, size_t maxBytes, const char* note = "\n... (truncated)");

}  // namespace chat
