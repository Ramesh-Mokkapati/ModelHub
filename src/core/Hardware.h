#pragma once
#include <cstdint>
#include <string>

namespace modelhub {

struct HardwareInfo {
    std::string gpuName;      // empty = no discrete GPU
    uint64_t vramMB = 0;      // dedicated video memory of the largest GPU (0 for integrated GPUs)
    uint64_t ramMB = 0;       // physical memory
    uint64_t availRamMB = 0;  // free right now
    unsigned cores = 0;       // logical processors
    bool avx2 = false;
};

// GPU via DXGI, RAM via GlobalMemoryStatusEx, AVX2 via IsProcessorFeaturePresent.
HardwareInfo DetectHardware();
std::string Describe(const HardwareInfo& hw);

}  // namespace modelhub
