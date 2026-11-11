#include "Paths.h"

#include <windows.h>

#include <cstdint>

namespace modelhub {

namespace fs = std::filesystem;

namespace {
std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf) : std::wstring();
}
}  // namespace

fs::path KnownFolderLocalAppData() {
    std::wstring v = Env(L"LOCALAPPDATA");
    return v.empty() ? fs::temp_directory_path() : fs::path(v);
}

fs::path HubHome() {
    std::wstring v = Env(L"MODELHUB_HOME");
    return v.empty() ? KnownFolderLocalAppData() / L"ModelHub" : fs::path(v);
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

uint64_t FreeDiskBytes(const fs::path& p) {
    fs::path probe = p;
    std::error_code ec;
    while (!probe.empty() && !fs::exists(probe, ec)) {
        if (probe == probe.parent_path()) break;
        probe = probe.parent_path();
    }
    ULARGE_INTEGER freeBytes{};
    if (!probe.empty() && GetDiskFreeSpaceExW(probe.c_str(), &freeBytes, nullptr, nullptr)) return freeBytes.QuadPart;
    return 0;
}

}  // namespace modelhub
