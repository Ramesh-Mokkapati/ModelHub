#include "LessonGeneral.h"

#include "Text.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace modelhub
{

using nlohmann::json;

namespace
{

struct Token
{
    enum Kind
    {
        Space,
        Comment,
        Ident,
        String,
        Other
    } kind;

    std::string text;    // as written
    std::string prefix;  // a string literal's L, u8, u, U (R included for a raw one)
    std::string body;    // a string literal's text between its quotes
};

bool IdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// C and C++ (and near enough Java) tokens; what is not a name, string or comment is kept as it is.
std::vector<Token> Lex(const std::string& s)
{
    std::vector<Token> out;
    size_t i = 0;
    auto quoted = [&](size_t from, const std::string& prefix) -> size_t
    {
        // from: at the opening quote; returns the index after the closing one
        size_t j = from + 1;
        if (!prefix.empty() && prefix.back() == 'R')
        {
            const size_t open = s.find('(', j);
            if (open == std::string::npos)
            {
                return s.size();
            }

            const std::string close = ")" + s.substr(j, open - j) + "\"";
            const size_t end = s.find(close, open);
            return end == std::string::npos ? s.size() : end + close.size();
        }

        for (; j < s.size() && s[j] != '"' && s[j] != '\n'; ++j)
        {
            if (s[j] == '\\')
            {
                ++j;
            }
        }

        return std::min(s.size(), j + 1);
    };

    while (i < s.size())
    {
        const char c = s[i];
        if (c == '/' && i + 1 < s.size() && (s[i + 1] == '/' || s[i + 1] == '*'))
        {
            size_t end = s[i + 1] == '/' ? s.find('\n', i) : s.find("*/", i + 2);
            end = end == std::string::npos ? s.size() : (s[i + 1] == '/' ? end : end + 2);
            out.push_back({ Token::Comment, s.substr(i, end - i), "", "" });
            i = end;
        }
        else if (std::isspace(static_cast<unsigned char>(c)))
        {
            size_t j = i;
            while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j])))
            {
                ++j;
            }

            out.push_back({ Token::Space, s.substr(i, j - i), "", "" });
            i = j;
        }
        else if (IdentStart(c))
        {
            size_t j = i;
            while (j < s.size() && IdentChar(s[j]))
            {
                ++j;
            }

            const std::string word = s.substr(i, j - i);
            static const std::set<std::string> prefixes = { "L", "u8", "u", "U", "R", "LR", "u8R", "uR", "UR" };
            if (j < s.size() && s[j] == '"' && prefixes.count(word))
            {
                const size_t end = quoted(j, word);
                Token t{ Token::String, s.substr(i, end - i), word, "" };
                if (word.back() != 'R' && end - j >= 2)
                {
                    t.body = s.substr(j + 1, end - j - 2);
                }

                out.push_back(t);
                i = end;
            }
            else
            {
                out.push_back({ Token::Ident, word, "", "" });
                i = j;
            }
        }
        else if (c == '"')
        {
            const size_t end = quoted(i, "");
            out.push_back({ Token::String, s.substr(i, end - i), "", end - i >= 2 ? s.substr(i + 1, end - i - 2) : "" });
            i = end;
        }
        else if (c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1]))))
        {
            size_t j = i + 1;
            for (; j < s.size() && s[j] != '\'' && s[j] != '\n'; ++j)
            {
                if (s[j] == '\\')
                {
                    ++j;
                }
            }

            j = std::min(s.size(), j + 1);
            out.push_back({ Token::Other, s.substr(i, j - i), "", "" });
            i = j;
        }
        else if (std::isdigit(static_cast<unsigned char>(c)))
        {
            size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == '\''))
            {
                ++j;
            }

            out.push_back({ Token::Other, s.substr(i, j - i), "", "" });
            i = j;
        }
        else
        {
            out.push_back({ Token::Other, std::string(1, c), "", "" });
            ++i;
        }
    }

    return out;
}

// Names everyone has: keywords, the C and C++ libraries, the Windows API, MFC and ATL. A name in ALL_CAPS (a macro, a
// constant, a Windows type: DWORD, LPCTSTR, MAX_PATH, CA2T) or with a leading underscore (_T, _tcscpy_s) is one too.
bool WellKnown(const std::string& name)
{
    static const std::set<std::string> names = {
        // C++ (and Java) keywords and types
        "alignas",
        "alignof",
        "and",
        "asm",
        "auto",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "char8_t",
        "char16_t",
        "char32_t",
        "class",
        "const",
        "consteval",
        "constexpr",
        "constinit",
        "const_cast",
        "continue",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "dynamic_cast",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "final",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "not",
        "nullptr",
        "operator",
        "or",
        "override",
        "private",
        "protected",
        "public",
        "register",
        "reinterpret_cast",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "static_assert",
        "static_cast",
        "struct",
        "switch",
        "template",
        "this",
        "thread_local",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
        "__int64",
        "__int32",
        "__cdecl",
        "__stdcall",
        "boolean",
        "byte",
        "extends",
        "implements",
        "import",
        "instanceof",
        "interface",
        "package",
        "super",
        "synchronized",
        "throws",
        "transient",
        "var",
        "null",
        "String",
        "Object",
        "Integer",
        "Long",
        "List",
        "ArrayList",
        "Map",
        "HashMap",
        "System",
        "out",
        "println",
        // the C and C++ libraries
        "std",
        "string",
        "wstring",
        "string_view",
        "vector",
        "map",
        "unordered_map",
        "set",
        "list",
        "deque",
        "pair",
        "tuple",
        "array",
        "optional",
        "unique_ptr",
        "shared_ptr",
        "make_unique",
        "make_shared",
        "make_pair",
        "move",
        "forward",
        "swap",
        "size_t",
        "ptrdiff_t",
        "intptr_t",
        "uintptr_t",
        "int8_t",
        "int16_t",
        "int32_t",
        "int64_t",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "uint64_t",
        "FILE",
        "printf",
        "sprintf",
        "sprintf_s",
        "snprintf",
        "fprintf",
        "wprintf",
        "swprintf",
        "swprintf_s",
        "vsprintf",
        "vsnprintf",
        "scanf",
        "sscanf",
        "sscanf_s",
        "strcpy",
        "strcpy_s",
        "strncpy",
        "strncpy_s",
        "strcat",
        "strcat_s",
        "strncat",
        "strlen",
        "strnlen",
        "strcmp",
        "strncmp",
        "stricmp",
        "strstr",
        "strchr",
        "strrchr",
        "strtok",
        "strtok_s",
        "strtol",
        "strtoul",
        "strdup",
        "atoi",
        "atol",
        "atof",
        "itoa",
        "memcpy",
        "memcpy_s",
        "memset",
        "memcmp",
        "memmove",
        "malloc",
        "calloc",
        "realloc",
        "free",
        "fopen",
        "fopen_s",
        "fclose",
        "fread",
        "fwrite",
        "fgets",
        "fputs",
        "fseek",
        "ftell",
        "fflush",
        "wcslen",
        "wcscpy",
        "wcscpy_s",
        "wcscat",
        "wcscmp",
        "wcsncpy",
        "toupper",
        "tolower",
        "isdigit",
        "isalpha",
        "isalnum",
        "isspace",
        "abs",
        "min",
        "max",
        "time",
        "localtime",
        "strftime",
        "c_str",
        "data",
        "size",
        "length",
        "empty",
        "clear",
        "push_back",
        "emplace_back",
        "pop_back",
        "begin",
        "end",
        "find",
        "rfind",
        "substr",
        "append",
        "insert",
        "erase",
        "at",
        "front",
        "back",
        "reserve",
        "resize",
        "count",
        "first",
        "second",
        "get",
        "reset",
        "release",
        "to_string",
        "stoi",
        "stol",
        "stod",
        "cout",
        "cerr",
        "endl",
        "npos",
        "assign",
        "compare",
        // the Windows API (their A and W forms count too)
        "CreateFile",
        "CloseHandle",
        "ReadFile",
        "WriteFile",
        "GetLastError",
        "SetLastError",
        "MessageBox",
        "SendMessage",
        "PostMessage",
        "SetWindowText",
        "GetWindowText",
        "LoadLibrary",
        "GetProcAddress",
        "FreeLibrary",
        "GetModuleFileName",
        "GetModuleHandle",
        "CreateProcess",
        "WaitForSingleObject",
        "WaitForMultipleObjects",
        "CreateThread",
        "CreateEvent",
        "SetEvent",
        "ResetEvent",
        "CreateMutex",
        "ReleaseMutex",
        "Sleep",
        "GetTickCount",
        "GetTickCount64",
        "MultiByteToWideChar",
        "WideCharToMultiByte",
        "CharUpperBuff",
        "CharLowerBuff",
        "CharUpper",
        "CharLower",
        "lstrlen",
        "lstrcpy",
        "lstrcpyn",
        "lstrcat",
        "lstrcmp",
        "lstrcmpi",
        "wsprintf",
        "wvsprintf",
        "OutputDebugString",
        "RegOpenKey",
        "RegOpenKeyEx",
        "RegQueryValueEx",
        "RegSetValueEx",
        "RegCreateKeyEx",
        "RegCloseKey",
        "RegDeleteKey",
        "RegDeleteValue",
        "RegEnumKeyEx",
        "RegEnumValue",
        "FindFirstFile",
        "FindNextFile",
        "FindClose",
        "DeleteFile",
        "CopyFile",
        "MoveFile",
        "MoveFileEx",
        "CreateDirectory",
        "RemoveDirectory",
        "GetFileAttributes",
        "SetFileAttributes",
        "GetFileSize",
        "GetFileSizeEx",
        "SetFilePointer",
        "ShellExecute",
        "ShellExecuteEx",
        "SHGetFolderPath",
        "SHGetSpecialFolderPath",
        "SHFileOperation",
        "GetTempPath",
        "GetTempFileName",
        "GetCurrentDirectory",
        "SetCurrentDirectory",
        "GetFullPathName",
        "GetPrivateProfileString",
        "WritePrivateProfileString",
        "GetPrivateProfileInt",
        "FormatMessage",
        "LocalFree",
        "LocalAlloc",
        "GlobalAlloc",
        "GlobalFree",
        "GlobalLock",
        "GlobalUnlock",
        "HeapAlloc",
        "HeapFree",
        "GetProcessHeap",
        "VirtualAlloc",
        "VirtualFree",
        "SysAllocString",
        "SysFreeString",
        "SysStringLen",
        "CoInitialize",
        "CoInitializeEx",
        "CoUninitialize",
        "CoCreateInstance",
        "QueryInterface",
        "AddRef",
        "Release",
        "SetWindowLong",
        "GetWindowLong",
        "SetWindowLongPtr",
        "GetWindowLongPtr",
        "SetClassLong",
        "GetClassLong",
        "SetClassLongPtr",
        "GetClassLongPtr",
        "DefWindowProc",
        "CallWindowProc",
        "RegisterClass",
        "RegisterClassEx",
        "CreateWindow",
        "CreateWindowEx",
        "DestroyWindow",
        "ShowWindow",
        "UpdateWindow",
        "InvalidateRect",
        "GetDlgItem",
        "GetDlgItemText",
        "SetDlgItemText",
        "GetDlgItemInt",
        "SetDlgItemInt",
        "EnableWindow",
        "IsWindow",
        "GetParent",
        "SetFocus",
        "LoadCursor",
        "LoadIcon",
        "LoadString",
        "LoadImage",
        "GetClientRect",
        "GetWindowRect",
        "MoveWindow",
        "SetWindowPos",
        "BeginPaint",
        "EndPaint",
        "GetDC",
        "ReleaseDC",
        "TextOut",
        "DrawText",
        "CreateFont",
        "CreateFontIndirect",
        "SelectObject",
        "DeleteObject",
        "GetSystemTime",
        "GetLocalTime",
        "SystemTimeToFileTime",
        "FileTimeToSystemTime",
        "GetComputerName",
        "GetUserName",
        "GetEnvironmentVariable",
        "SetEnvironmentVariable",
        "ExpandEnvironmentStrings",
        "GetVersionEx",
        "OpenProcess",
        "TerminateProcess",
        "GetExitCodeProcess",
        "CreatePipe",
        "PeekNamedPipe",
        "InternetOpen",
        "InternetOpenUrl",
        "InternetReadFile",
        "InternetCloseHandle",
        "HttpOpenRequest",
        "HttpSendRequest",
        "WSAStartup",
        "WSACleanup",
        "socket",
        "connect",
        "send",
        "recv",
        "closesocket",
        "bind",
        "listen",
        "accept",
        "inet_addr",
        "htons",
        "ntohs",
        "gethostbyname",
        "getaddrinfo",
        "SQLDriverConnect",
        "SQLConnect",
        "SQLExecDirect",
        "SQLPrepare",
        "SQLExecute",
        "SQLFetch",
        "SQLGetData",
        "SQLBindCol",
        "SQLBindParameter",
        "SQLAllocHandle",
        "SQLFreeHandle",
        "SQLGetDiagRec",
        "SQLDisconnect",
        "AVIFileOpen",
        "AVIFileCreateStream",
        "AVIStreamWrite",
        "AVIStreamRelease",
        "AVIFileRelease",
        "Process32First",
        "Process32Next",
        "Module32First",
        "Module32Next",
        "CreateToolhelp32Snapshot",
        // MFC and ATL
        "CString",
        "CStringA",
        "CStringW",
        "CStringT",
        "CSimpleString",
        "CWnd",
        "CDialog",
        "CDialogEx",
        "CFrameWnd",
        "CView",
        "CDocument",
        "CWinApp",
        "CFile",
        "CStdioFile",
        "CFileFind",
        "CTime",
        "CTimeSpan",
        "COleDateTime",
        "COleDateTimeSpan",
        "COleVariant",
        "CArray",
        "CStringArray",
        "CMap",
        "CList",
        "CPtrList",
        "CObList",
        "CObject",
        "CPoint",
        "CRect",
        "CSize",
        "CDC",
        "CPaintDC",
        "CClientDC",
        "CBrush",
        "CPen",
        "CFont",
        "CBitmap",
        "CImage",
        "CEdit",
        "CButton",
        "CStatic",
        "CListBox",
        "CComboBox",
        "CListCtrl",
        "CTreeCtrl",
        "CComPtr",
        "CComQIPtr",
        "CComBSTR",
        "CComVariant",
        "CRegKey",
        "CHandle",
        "CEvent",
        "CMutex",
        "CCriticalSection",
        "CSingleLock",
        "CWinThread",
        "AfxMessageBox",
        "AfxGetApp",
        "AfxGetMainWnd",
        "AfxBeginThread",
        "AfxGetInstanceHandle",
        "AfxGetResourceHandle",
        "Format",
        "FormatV",
        "AppendFormat",
        "GetBuffer",
        "GetBufferSetLength",
        "ReleaseBuffer",
        "GetLength",
        "GetString",
        "GetAt",
        "SetAt",
        "Left",
        "Right",
        "Mid",
        "Find",
        "FindOneOf",
        "ReverseFind",
        "Replace",
        "Remove",
        "Insert",
        "Delete",
        "Trim",
        "TrimLeft",
        "TrimRight",
        "MakeUpper",
        "MakeLower",
        "MakeReverse",
        "IsEmpty",
        "Empty",
        "Compare",
        "CompareNoCase",
        "Append",
        "AllocSysString",
        "Tokenize",
        "SpanIncluding",
        "SpanExcluding",
        "ParseDateTime",
        "GetCurrentTime",
        "GetYear",
        "GetMonth",
        "GetDay",
        "GetHour",
        "GetMinute",
        "GetSecond",
        "Create",
        "GetSafeHwnd",
        "m_hWnd",
        "Open",
        "Close",
        "Read",
        "Write",
        "ReadString",
        "WriteString",
        "Seek",
        "GetPosition",
        "Add",
        "GetSize",
        "GetCount",
        "RemoveAll",
        "RemoveAt",
        "SetSize",
        "Lookup",
        "SetAt",
        "GetHead",
        "GetTail",
        "AddTail",
        "AddHead",
        "Attach",
        "Detach",
        "Lock",
        "Unlock",
        "SetWindowTextW",
        "UpdateData",
        "OnInitDialog",
        "OnOK",
        "OnCancel",
        "DoModal",
        "EndDialog",
        "Invalidate",
        "RedrawWindow",
        "GetWindowTextLength",
        "SetTimer",
        "KillTimer",
        "MessageBeep",
    };

    if (name.empty())
    {
        return true;
    }

    if (name[0] == '_')
    {
        return true;
    }

    bool caps = false, lower = false;
    for (char c : name)
    {
        caps = caps || std::isupper(static_cast<unsigned char>(c));
        lower = lower || std::islower(static_cast<unsigned char>(c));
    }

    if (caps && !lower && name.size() >= 2)
    {
        return true;  // ALL_CAPS
    }

    if (names.count(name))
    {
        return true;
    }

    // the A and W forms of a Windows name, and the _s forms of a CRT one
    if (name.size() > 1 && (name.back() == 'A' || name.back() == 'W') && names.count(name.substr(0, name.size() - 1)))
    {
        return true;
    }

    if (name.size() > 2 && name.compare(name.size() - 2, 2, "_s") == 0 && names.count(name.substr(0, name.size() - 2)))
    {
        return true;
    }

    return false;
}

std::vector<std::string> Parts(const json& v)
{
    std::vector<std::string> out;
    if (v.is_string())
    {
        out.push_back(v.get<std::string>());
    }
    else if (v.is_array())
    {
        for (const auto& x : v)
        {
            if (x.is_string())
            {
                out.push_back(x.get<std::string>());
            }
        }
    }

    return out;
}

bool IsPlaceholder(const std::string& name, const char* stem)
{
    const std::string s(stem);
    if (name.size() <= s.size() || name.compare(0, s.size(), s) != 0)
    {
        return false;
    }

    for (size_t i = s.size(); i < name.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
        {
            return false;
        }
    }

    return true;
}

std::string RightTrimLines(const std::string& s)
{
    std::string out;
    size_t from = 0;
    while (from <= s.size())
    {
        size_t nl = s.find('\n', from);
        std::string line = s.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        size_t e = line.find_last_not_of(" \t\r");
        line = e == std::string::npos ? std::string() : line.substr(0, e + 1);
        out += line;
        if (nl == std::string::npos)
        {
            break;
        }

        out += "\n";
        from = nl + 1;
    }

    return out;
}

}  // namespace

size_t LessonLines(const json& record)
{
    size_t most = 0;
    for (const char* field : { "removed", "added" })
    {
        if (!record.contains(field))
        {
            continue;
        }

        const json& v = record[field];
        size_t n = 0;
        if (v.is_array())
        {
            n = v.size();
        }
        else if (v.is_string())
        {
            const std::string s = v.get<std::string>();
            n = s.empty() ? 0 : 1 + static_cast<size_t>(std::count(s.begin(), s.end(), '\n')) - (s.back() == '\n' ? 1 : 0);
        }

        most = std::max(most, n);
    }

    return most;
}

json GeneralLesson(const json& record)
{
    if (!record.is_object() || record.value("general", false))
    {
        return record;
    }

    json r = record;
    const std::vector<std::string> removed = record.contains("removed") ? Parts(record["removed"]) : std::vector<std::string>{};
    const std::vector<std::string> added = record.contains("added") ? Parts(record["added"]) : std::vector<std::string>{};
    std::vector<std::vector<Token>> rem, add;
    for (const auto& s : removed)
    {
        rem.push_back(Lex(s));
    }

    for (const auto& s : added)
    {
        add.push_back(Lex(s));
    }

    std::set<std::string> namesAdded, textsAdded;
    for (const auto& line : add)
    {
        for (const auto& t : line)
        {
            if (t.kind == Token::Ident)
            {
                namesAdded.insert(t.text);
            }
            else if (t.kind == Token::String && !t.body.empty())
            {
                textsAdded.insert(t.body);
            }
        }
    }

    // the program's own names and texts that the fix keeps, numbered in the order the removed lines show them
    std::map<std::string, std::string> names, texts;
    for (const auto& line : rem)
    {
        for (const auto& t : line)
        {
            if (t.kind == Token::Ident && namesAdded.count(t.text) && !WellKnown(t.text) && !IsPlaceholder(t.text, "name") &&
                !names.count(t.text))
            {
                names[t.text] = "name" + std::to_string(names.size() + 1);
            }
            else if (t.kind == Token::String && !t.body.empty() && textsAdded.count(t.body) &&
                     t.body.find('%') == std::string::npos && !IsPlaceholder(t.body, "text") && !texts.count(t.body))
            {
                texts[t.body] = "text" + std::to_string(texts.size() + 1);
            }
        }
    }

    auto rebuild = [&](const std::vector<Token>& line)
    {
        std::string out;
        for (const auto& t : line)
        {
            if (t.kind == Token::Comment)
            {
                continue;  // the program's own words
            }

            if (t.kind == Token::Ident)
            {
                auto it = names.find(t.text);
                out += it == names.end() ? t.text : it->second;
            }
            else if (t.kind == Token::String && texts.count(t.body))
            {
                out += t.prefix + "\"" + texts[t.body] + "\"";
            }
            else
            {
                out += t.text;
            }
        }

        return RightTrimLines(out);
    };

    auto write = [&](const char* field, const std::vector<std::vector<Token>>& lines)
    {
        if (!record.contains(field))
        {
            return;
        }

        if (record[field].is_string())
        {
            r[field] = lines.empty() ? std::string() : rebuild(lines.front());
        }
        else if (record[field].is_array())
        {
            json list = json::array();
            for (const auto& l : lines)
            {
                list.push_back(rebuild(l));
            }

            r[field] = list;
        }
    };

    write("removed", rem);
    write("added", add);

    // each side from column 0: the indentation was the program's (an application replaying a lesson indents it as the
    // file around it), and two records that differ only in it are the same lesson
    for (const char* field : { "removed", "added" })
    {
        if (!r.contains(field))
        {
            continue;
        }

        json& v = r[field];
        std::vector<std::string> parts;
        if (v.is_string())
        {
            parts = SplitLines(v.get<std::string>());
        }
        else if (v.is_array())
        {
            for (const auto& x : v)
            {
                parts.push_back(x.is_string() ? x.get<std::string>() : std::string());
            }
        }

        std::string indent;
        bool found = false;
        for (const auto& line : parts)
        {
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos)
            {
                continue;
            }

            indent = line.substr(0, first);
            found = true;
            break;
        }

        if (!found || indent.empty())
        {
            continue;
        }

        for (auto& line : parts)
        {
            if (line.compare(0, indent.size(), indent) == 0)
            {
                line = line.substr(indent.size());
            }
        }

        if (v.is_string())
        {
            std::string joined;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                joined += (i ? "\n" : "") + parts[i];
            }

            v = joined;
        }
        else
        {
            v = parts;
        }
    }

    // the message names the same things: the same placeholders there
    for (const char* field : { "error", "detail", "note" })
    {
        if (!record.contains(field) || !record[field].is_string() || names.empty())
        {
            continue;
        }

        const std::string m = record[field].get<std::string>();
        std::string out;
        for (size_t i = 0; i < m.size();)
        {
            if (IdentStart(m[i]) && (i == 0 || !IdentChar(m[i - 1])))
            {
                size_t j = i;
                while (j < m.size() && IdentChar(m[j]))
                {
                    ++j;
                }

                const std::string word = m.substr(i, j - i);
                auto it = names.find(word);
                out += it == names.end() ? word : it->second;
                i = j;
            }
            else
            {
                out += m[i++];
            }
        }

        r[field] = out;
    }

    r["general"] = true;
    return r;
}

}  // namespace modelhub
