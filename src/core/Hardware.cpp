#include "Hardware.h"

#include <windows.h>
#include <dxgi.h>

#include <cstdio>

#include "Text.h"

namespace modelhub {

HardwareInfo DetectHardware() {
    HardwareInfo hw;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                uint64_t mb = static_cast<uint64_t>(d.DedicatedVideoMemory) >> 20;
                if (mb > hw.vramMB) {
                    hw.vramMB = mb;
                    hw.gpuName = Narrow(d.Description);
                }
            }
            adapter->Release();
        }
        factory->Release();
    }
    if (hw.vramMB < 1024) {  // integrated GPUs report a token amount of dedicated memory
        hw.vramMB = 0;
        if (!hw.gpuName.empty()) hw.gpuName += " (integrated: treated as CPU)";
    }
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        hw.ramMB = ms.ullTotalPhys >> 20;
        hw.availRamMB = ms.ullAvailPhys >> 20;
    }
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    hw.cores = si.dwNumberOfProcessors;
    hw.avx2 = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != FALSE;
    return hw;
}

std::string Describe(const HardwareInfo& hw) {
    char b[256];
    std::snprintf(b, sizeof b, "%s, %.1f GB RAM (%.1f GB free), %u logical processors, AVX2 %s",
                  hw.vramMB ? (hw.gpuName + " " + std::to_string((hw.vramMB + 512) / 1024) + " GB").c_str()
                            : (hw.gpuName.empty() ? std::string("no GPU") : hw.gpuName).c_str(),
                  hw.ramMB / 1024.0, hw.availRamMB / 1024.0, hw.cores, hw.avx2 ? "yes" : "no");
    return b;
}

}  // namespace modelhub
