#pragma once
// Which model to use for a task on this PC: the PC's resources, the models already installed, and the task's
// workload. Same rules for every task; the task profile in the catalog holds what differs.
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Catalog.h"
#include "Hardware.h"

namespace modelhub {

// How the application runs the model:
//   Hub         - through the hub's llama.cpp server (GPU when there is one).
//   EmbeddedCpu - in the application's own process on the CPU (e.g. a llama.cpp built CPU-only); GPU ignored.
enum class Engine { Hub, EmbeddedCpu };
Engine ParseEngine(const std::string& s);  // "hub" (default) or "embedded-cpu"
const char* ToString(Engine e);

enum class Fit {
    Gpu,             // all layers on the GPU
    PartialOffload,  // part on the GPU, the rest on the CPU: works, slower
    Cpu,             // CPU only, leaves a quarter of the RAM free
    CpuTight,        // CPU only, close other programs first
    TooLarge
};
const char* ToString(Fit f);

struct MemoryNeed {
    uint64_t totalMB = 0;    // weights + KV cache for all slots + runtime headroom
    uint64_t weightsMB = 0;
    uint64_t kvMB = 0;
};
MemoryNeed NeedFor(const CatalogModel& m, int contextTokens, int parallel);
Fit FitFor(const CatalogModel& m, const HardwareInfo& hw, int contextTokens, int parallel, Engine engine);
// llama-server --n-gpu-layers: 999 when everything fits, a share on partial offload, 0 on CPU.
int GpuLayersFor(const CatalogModel& m, const HardwareInfo& hw, int contextTokens, int parallel);

struct Workload {
    std::map<std::string, double> metrics;  // compared with TaskTier::below
    std::string summary;                    // e.g. "2276 lines of code in 11 files"
};

struct ModelOption {
    const CatalogModel* model = nullptr;
    Fit fit = Fit::TooLarge;
    MemoryNeed need;
    bool installed = false;
    bool fitsDisk = false;  // installed, or enough free disk to download it
};

struct Advice {
    const TaskProfile* task = nullptr;
    std::vector<ModelOption> options;          // the task's models, smallest first
    const CatalogModel* firstModel = nullptr;  // smallest that runs comfortably
    const CatalogModel* recommended = nullptr; // best for the workload that fits
    const CatalogModel* ideal = nullptr;       // the tier's first choice, if different
    const TaskTier* tier = nullptr;
    int tierIndex = 0;                         // 1-based, 0 = no workload
    bool recommendedInstalled = false;
    bool upgradeSuggested = false;             // `current` is a lower capability tier than `recommended`
    std::vector<std::string> notes;            // plain-language reasons
};

using IsInstalledFn = std::function<bool(const CatalogModel&)>;

// workload may be null (nothing to measure yet). currentModel: the id the application uses now, if any.
Advice Advise(const Catalog& catalog, const TaskProfile& task, const HardwareInfo& hw, Engine engine,
              const Workload* workload, const std::string& currentModel, uint64_t freeDiskBytes,
              const IsInstalledFn& isInstalled);

const TaskTier* ClassifyWorkload(const TaskProfile& task, const Workload& w, int* index = nullptr);
std::string FormatSize(uint64_t bytes);
// "fast", "moderate" or "slow": generation speed follows the parameters used per token, and the GPU.
const char* SpeedLabel(const CatalogModel& m, const HardwareInfo& hw, Fit fit);

}  // namespace modelhub
