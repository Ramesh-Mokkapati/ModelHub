#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace modelhub {

struct ProcessSpec {
    std::wstring exe;                        // full path or a name resolved through PATH (.exe/.cmd/.bat)
    std::vector<std::wstring> args;
    std::filesystem::path workDir;           // empty = current directory
    std::map<std::wstring, std::wstring> env;  // added to / overriding the parent environment
    uint64_t memoryLimitMB = 0;              // per-process limit via Job Object, 0 = none
    std::chrono::seconds timeout{0};         // 0 = no timeout (RunProcess only)
};

struct ProcessResult {
    int exitCode = -1;
    bool launchFailed = false;
    bool timedOut = false;
    bool cancelled = false;
    std::string error;
    bool Succeeded() const { return !launchFailed && !timedOut && !cancelled && exitCode == 0; }
};

// Called once per output line (without the line break), from reader threads, serialized.
using LineCallback = std::function<void(std::string_view line, bool isStdErr)>;

// Resolves "java" / "ilspycmd" / a full path to an executable. Returns empty if not found.
std::wstring ResolveExecutable(std::wstring_view exe);

// Builds the CreateProcess command line; .cmd/.bat go through cmd.exe /d /s /c.
std::wstring BuildCommandLine(const std::wstring& resolvedExe, const std::vector<std::wstring>& args);

// Starts a program the user keeps working in (e.g. the Ghidra GUI): not tied to this app's lifetime,
// no pipes, no console window. Returns false with *error when it cannot be started.
bool LaunchDetached(const ProcessSpec& spec, std::string* error);

// Runs to completion. All child processes live in a Job Object and die on timeout/cancel.
ProcessResult RunProcess(const ProcessSpec& spec, const LineCallback& onLine, std::stop_token stop = {});

// A long-running child (Ghidra server, MCP bridge). Killed with its whole tree on Terminate/destruction.
class ManagedProcess {
public:
    ManagedProcess();
    ~ManagedProcess();
    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;

    bool Start(const ProcessSpec& spec, LineCallback onLine, std::string* error);
    bool IsRunning() const;
    uint32_t Pid() const;
    void Terminate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace modelhub
