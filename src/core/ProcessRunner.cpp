#include "ProcessRunner.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <thread>

#include "Text.h"

namespace modelhub {
namespace {

std::string LastErrorText(const char* what) {
    DWORD code = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string text = msg ? Trim(Narrow(msg)) : std::string();
    if (msg) LocalFree(msg);
    return std::string(what) + " failed (" + std::to_string(code) + "): " + text;
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h(o.h) { o.h = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            Reset();
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    void Reset() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

struct Launched {
    Handle job, process, outRead, errRead;
    DWORD pid = 0;
};

std::wstring BuildEnvironmentBlock(const std::map<std::wstring, std::wstring>& overrides) {
    // Case-insensitive merge of the parent environment with the overrides.
    std::map<std::wstring, std::wstring, decltype([](const std::wstring& a, const std::wstring& b) {
                 return _wcsicmp(a.c_str(), b.c_str()) < 0;
             })>
        vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            size_t eq = entry.find(L'=', 1);  // entries like "=C:=C:\" start with '='
            if (eq == std::wstring::npos) continue;
            vars[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [k, v] : overrides) vars[k] = v;
    std::wstring out;
    for (const auto& [k, v] : vars) {
        out += k;
        out += L'=';
        out += v;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

bool Launch(const ProcessSpec& spec, Launched& L, std::string* error) {
    std::wstring exe = ResolveExecutable(spec.exe);
    if (exe.empty()) {
        if (error) *error = "executable not found: " + Narrow(spec.exe);
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE outR, outW, errR, errW;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        if (error) *error = LastErrorText("CreatePipe");
        return false;
    }
    L.outRead = Handle(outR);
    L.errRead = Handle(errR);
    Handle outWrite(outW), errWrite(errW);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    Handle nulIn(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));

    L.job = Handle(CreateJobObjectW(nullptr, nullptr));
    if (!L.job) {
        if (error) *error = LastErrorText("CreateJobObject");
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (spec.memoryLimitMB) {
        lim.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        lim.ProcessMemoryLimit = static_cast<SIZE_T>(spec.memoryLimitMB) * 1024 * 1024;
    }
    SetInformationJobObject(L.job.h, JobObjectExtendedLimitInformation, &lim, sizeof lim);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nulIn.h;
    si.StartupInfo.hStdOutput = outWrite.h;
    si.StartupInfo.hStdError = errWrite.h;

    // Only the three std handles are inherited by the child.
    HANDLE inherit[3] = {nulIn.h, outWrite.h, errWrite.h};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr,
                              nullptr);

    std::wstring cmd = BuildCommandLine(exe, spec.args);
    std::wstring envBlock = BuildEnvironmentBlock(spec.env);
    std::wstring cwd = spec.workDir.empty() ? std::wstring() : spec.workDir.wstring();

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT,
                             envBlock.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) {
        if (error) *error = LastErrorText("CreateProcess") + " for " + Narrow(cmd);
        return false;
    }
    AssignProcessToJobObject(L.job.h, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    L.process = Handle(pi.hProcess);
    L.pid = pi.dwProcessId;
    return true;  // write ends close here, so readers see EOF when the child exits
}

void ReadLines(HANDLE pipe, bool isErr, const LineCallback& cb, std::mutex& cbMutex) {
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    auto emit = [&](std::string_view line) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!cb) return;
        std::lock_guard lk(cbMutex);
        cb(line, isErr);
    };
    while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got > 0) {
        pending.append(buf, got);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            emit(std::string_view(pending).substr(0, pos));
            pending.erase(0, pos + 1);
        }
        if (pending.size() > 1 << 20) {  // pathological line without breaks
            emit(pending);
            pending.clear();
        }
    }
    if (!pending.empty()) emit(pending);
}

}  // namespace

std::wstring ResolveExecutable(std::wstring_view exe) {
    if (exe.empty()) return {};
    std::filesystem::path p(exe);
    std::error_code ec;
    if (p.has_parent_path()) {
        if (std::filesystem::is_regular_file(p, ec)) return p.wstring();
        for (const wchar_t* ext : {L".exe", L".cmd", L".bat"}) {
            auto q = p;
            q += ext;
            if (std::filesystem::is_regular_file(q, ec)) return q.wstring();
        }
        return {};
    }
    std::wstring name(exe);
    for (const wchar_t* ext : {L"", L".exe", L".cmd", L".bat"}) {
        if (*ext == 0 && name.find(L'.') == std::wstring::npos) continue;
        wchar_t buf[MAX_PATH * 2];
        DWORD n = SearchPathW(nullptr, name.c_str(), *ext ? ext : nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr);
        if (n > 0 && n < std::size(buf)) return buf;
    }
    return {};
}

std::wstring BuildCommandLine(const std::wstring& resolvedExe, const std::vector<std::wstring>& args) {
    std::string lower = ToLower(Narrow(resolvedExe));
    bool script = EndsWithI(lower, ".cmd") || EndsWithI(lower, ".bat");
    std::wstring inner = QuoteArg(resolvedExe);
    for (const auto& a : args) {
        inner += L' ';
        inner += QuoteArg(a);
    }
    if (!script) return inner;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    // /s strips the outer quote pair and runs the rest verbatim.
    return QuoteArg(std::wstring(sys) + L"\\cmd.exe") + L" /d /s /c \"" + inner + L"\"";
}

bool LaunchDetached(const ProcessSpec& spec, std::string* error) {
    std::wstring exe = ResolveExecutable(spec.exe);
    if (exe.empty()) {
        if (error) *error = "executable not found: " + Narrow(spec.exe);
        return false;
    }
    std::wstring cmd = BuildCommandLine(exe, spec.args);
    std::wstring envBlock = BuildEnvironmentBlock(spec.env);
    std::wstring cwd = spec.workDir.empty() ? std::filesystem::path(exe).parent_path().wstring() : spec.workDir.wstring();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP, envBlock.data(),
                        cwd.c_str(), &si, &pi)) {
        if (error) *error = LastErrorText("CreateProcess") + " for " + Narrow(cmd);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

ProcessResult RunProcess(const ProcessSpec& spec, const LineCallback& onLine, std::stop_token stop) {
    ProcessResult r;
    Launched L;
    if (!Launch(spec, L, &r.error)) {
        r.launchFailed = true;
        return r;
    }
    std::mutex cbMutex;
    std::thread tOut(ReadLines, L.outRead.h, false, std::cref(onLine), std::ref(cbMutex));
    std::thread tErr(ReadLines, L.errRead.h, true, std::cref(onLine), std::ref(cbMutex));

    auto start = std::chrono::steady_clock::now();
    for (;;) {
        DWORD w = WaitForSingleObject(L.process.h, 200);
        if (w == WAIT_OBJECT_0) break;
        if (stop.stop_requested()) {
            r.cancelled = true;
            TerminateJobObject(L.job.h, 1);
            break;
        }
        if (spec.timeout.count() > 0 && std::chrono::steady_clock::now() - start > spec.timeout) {
            r.timedOut = true;
            TerminateJobObject(L.job.h, 1);
            break;
        }
    }
    WaitForSingleObject(L.process.h, 5000);
    // Grandchildren may still hold the pipe open; killing the job closes them.
    TerminateJobObject(L.job.h, 1);
    tOut.join();
    tErr.join();
    DWORD code = 0;
    GetExitCodeProcess(L.process.h, &code);
    r.exitCode = static_cast<int>(code);
    return r;
}

struct ManagedProcess::Impl {
    Launched L;
    std::mutex cbMutex;
    LineCallback cb;
    std::thread tOut, tErr;
};

ManagedProcess::ManagedProcess() : impl_(std::make_unique<Impl>()) {}
ManagedProcess::~ManagedProcess() { Terminate(); }

bool ManagedProcess::Start(const ProcessSpec& spec, LineCallback onLine, std::string* error) {
    Terminate();
    impl_ = std::make_unique<Impl>();
    impl_->cb = std::move(onLine);
    if (!Launch(spec, impl_->L, error)) return false;
    impl_->tOut = std::thread(ReadLines, impl_->L.outRead.h, false, std::cref(impl_->cb), std::ref(impl_->cbMutex));
    impl_->tErr = std::thread(ReadLines, impl_->L.errRead.h, true, std::cref(impl_->cb), std::ref(impl_->cbMutex));
    return true;
}

bool ManagedProcess::IsRunning() const {
    return impl_ && impl_->L.process && WaitForSingleObject(impl_->L.process.h, 0) == WAIT_TIMEOUT;
}

uint32_t ManagedProcess::Pid() const { return impl_ ? impl_->L.pid : 0; }

void ManagedProcess::Terminate() {
    if (!impl_) return;
    if (impl_->L.job) TerminateJobObject(impl_->L.job.h, 1);
    if (impl_->tOut.joinable()) impl_->tOut.join();
    if (impl_->tErr.joinable()) impl_->tErr.join();
    impl_->L = Launched{};
}

}  // namespace modelhub
