#include "Advisor.h"

#include <algorithm>
#include <cstdio>

namespace modelhub {

namespace {

constexpr uint64_t kHeadroomMB = 1024;  // runtime buffers, compute graph

std::string Gb(uint64_t mb) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f GB", mb / 1024.0);
    return b;
}

bool Comfortable(Fit f, const HardwareInfo& hw, Engine engine) {
    bool gpu = engine == Engine::Hub && hw.vramMB > 0;
    return gpu ? f == Fit::Gpu : f == Fit::Cpu;
}

}  // namespace

Engine ParseEngine(const std::string& s) { return s == "embedded-cpu" ? Engine::EmbeddedCpu : Engine::Hub; }
const char* ToString(Engine e) { return e == Engine::EmbeddedCpu ? "embedded-cpu" : "hub"; }

const char* ToString(Fit f) {
    switch (f) {
        case Fit::Gpu: return "gpu";
        case Fit::PartialOffload: return "gpu+cpu";
        case Fit::Cpu: return "cpu";
        case Fit::CpuTight: return "cpu-tight";
        default: return "too-large";
    }
}

MemoryNeed NeedFor(const CatalogModel& m, int contextTokens, int parallel) {
    MemoryNeed n;
    n.weightsMB = (m.SizeBytes() >> 20) * 115 / 100;
    uint64_t kvPerSlot = m.kvCacheMB8k * static_cast<uint64_t>(std::max(contextTokens, 512)) / 8192;
    n.kvMB = kvPerSlot * static_cast<uint64_t>(std::max(1, parallel));
    n.totalMB = n.weightsMB + n.kvMB + kHeadroomMB;
    return n;
}

Fit FitFor(const CatalogModel& m, const HardwareInfo& hw, int contextTokens, int parallel, Engine engine) {
    uint64_t need = NeedFor(m, contextTokens, parallel).totalMB;
    if (engine == Engine::Hub && hw.vramMB > 0) {
        uint64_t vram = hw.vramMB * 92 / 100;  // leave room for the desktop and the driver
        if (need <= vram) return Fit::Gpu;
        if (need <= vram + hw.ramMB * 60 / 100) return Fit::PartialOffload;
        return Fit::TooLarge;
    }
    if (need <= hw.ramMB * 75 / 100) return Fit::Cpu;
    if (need <= hw.ramMB * 90 / 100) return Fit::CpuTight;
    return Fit::TooLarge;
}

int GpuLayersFor(const CatalogModel& m, const HardwareInfo& hw, int contextTokens, int parallel) {
    Fit f = FitFor(m, hw, contextTokens, parallel, Engine::Hub);
    if (f == Fit::Gpu) return 999;  // llama.cpp clamps to the real layer count
    if (f != Fit::PartialOffload || m.layers <= 0) return 0;
    MemoryNeed n = NeedFor(m, contextTokens, parallel);
    int64_t room = static_cast<int64_t>(hw.vramMB * 92 / 100) - static_cast<int64_t>(n.kvMB + kHeadroomMB);
    if (room <= 0) return 0;
    int layers = static_cast<int>(room / std::max<int64_t>(1, static_cast<int64_t>(n.weightsMB) / m.layers));
    return std::clamp(layers, 0, m.layers);
}

const TaskTier* ClassifyWorkload(const TaskProfile& task, const Workload& w, int* index) {
    for (size_t t = 0; t < task.tiers.size(); ++t) {
        bool within = true;
        for (const auto& [metric, limit] : task.tiers[t].below) {
            auto it = w.metrics.find(metric);
            within = within && (it == w.metrics.end() || it->second < limit);
        }
        if (within || t + 1 == task.tiers.size()) {
            if (index) *index = static_cast<int>(t) + 1;
            return &task.tiers[t];
        }
    }
    return nullptr;
}

const char* SpeedLabel(const CatalogModel& m, const HardwareInfo& hw, Fit fit) {
    if (fit == Fit::Gpu) return m.activeParamsB <= 16 ? "fast" : "moderate";
    double work = m.activeParamsB * (hw.cores < 8 ? 2.0 : 1.0) * (fit == Fit::PartialOffload ? 0.7 : 1.0);
    return work <= 4.0 ? "fast" : work <= 9.0 ? "moderate" : "slow";
}

std::string FormatSize(uint64_t bytes) {
    char b[32];
    if (bytes >= (1ull << 30))
        std::snprintf(b, sizeof b, "%.1f GB", bytes / double(1ull << 30));
    else
        std::snprintf(b, sizeof b, "%.0f MB", bytes / double(1ull << 20));
    return b;
}

Advice Advise(const Catalog& catalog, const TaskProfile& task, const HardwareInfo& hw, Engine engine,
              const Workload* workload, const std::string& currentModel, uint64_t freeDiskBytes,
              const IsInstalledFn& isInstalled) {
    Advice a;
    a.task = &task;
    const int ctx = task.contextTokens, par = task.parallel;
    std::string where = engine == Engine::Hub && hw.vramMB > 0 ? Gb(hw.vramMB) + " of graphics memory"
                                                               : Gb(hw.ramMB) + " of RAM";
    if (engine == Engine::EmbeddedCpu && !hw.avx2) {
        a.notes.push_back("This processor has no AVX2, which CPU builds of llama.cpp need. Run models through the "
                          "hub instead (engine \"hub\").");
        return a;
    }

    for (const auto& m : catalog.models) {
        if (!task.UsesModel(m.id)) continue;
        ModelOption o;
        o.model = &m;
        o.need = NeedFor(m, ctx, par);
        o.fit = FitFor(m, hw, ctx, par, engine);
        o.installed = isInstalled && isInstalled(m);
        o.fitsDisk = o.installed || freeDiskBytes == 0 || m.SizeBytes() + (1ull << 30) <= freeDiskBytes;
        a.options.push_back(o);
    }
    auto option = [&](const CatalogModel* m) -> const ModelOption* {
        for (const auto& o : a.options)
            if (o.model == m) return &o;
        return nullptr;
    };

    for (const auto& o : a.options)
        if (!a.firstModel && Comfortable(o.fit, hw, engine)) a.firstModel = o.model;
    for (const auto& o : a.options)
        if (!a.firstModel && o.fit != Fit::TooLarge) a.firstModel = o.model;
    if (!a.firstModel) {
        a.notes.push_back("Even the smallest model for " + task.name + " (" +
                          Gb(a.options.empty() ? 0 : a.options.front().need.totalMB) + ") is too large for this PC's " +
                          where + ".");
        return a;
    }

    bool noWorkload = !workload || workload->metrics.empty();
    if (noWorkload && task.startSmall) {
        a.recommended = a.firstModel;
        a.notes.push_back("Start with " + a.firstModel->name + ": the smallest model for " + task.name +
                          ", and it runs comfortably here (needs about " + Gb(option(a.firstModel)->need.totalMB) +
                          " of your " + where + ").");
        if (!task.noWorkloadHint.empty()) a.notes.push_back(task.noWorkloadHint);
    } else {
        Workload none;
        a.tier = ClassifyWorkload(task, noWorkload ? none : *workload, &a.tierIndex);
        if (noWorkload) a.tierIndex = 0;
        const auto& prefer = a.tier->prefer;
        a.ideal = catalog.Find(prefer.front());
        for (int pass = 0; pass < 2 && !a.recommended; ++pass)
            for (const auto& id : prefer) {
                const ModelOption* o = option(catalog.Find(id));
                if (!o || !o->fitsDisk) continue;
                bool ok = pass == 0 ? Comfortable(o->fit, hw, engine) : o->fit != Fit::TooLarge;
                if (ok) {
                    a.recommended = o->model;
                    break;
                }
            }
        if (!a.recommended) a.recommended = a.firstModel;
        if (!noWorkload) {
            std::string summary = workload->summary;
            if (summary.empty())  // describe the metrics themselves
                for (const auto& [k, v] : workload->metrics) {
                    char b[96];
                    std::snprintf(b, sizeof b, "%s%s %.0f", summary.empty() ? "" : ", ", k.c_str(), v);
                    summary += b;
                }
            a.notes.push_back("Your " + task.workloadNoun + " is " + (a.tier->label.empty() ? "" : a.tier->label + " (") +
                              summary + (a.tier->label.empty() ? "" : ")") + ". " + a.tier->explain);
        }
        else if (!a.tier->explain.empty())
            a.notes.push_back(a.tier->explain);
        if (a.ideal && a.ideal != a.recommended) {
            const ModelOption* io = option(a.ideal);
            a.notes.push_back(a.ideal->name + " would suit it best but needs about " +
                              Gb(io ? io->need.totalMB : 0) + (io && !io->fitsDisk ? " and more free disk" : "") +
                              "; this PC has " + where + ". Recommending the best model that fits.");
        }
        const ModelOption* ro = option(a.recommended);
        a.notes.push_back("Recommended: " + a.recommended->name + " (" + FormatSize(a.recommended->SizeBytes()) +
                          ", needs about " + Gb(ro->need.totalMB) + ", " + ToString(ro->fit) + "). " +
                          a.recommended->summary);
    }

    const ModelOption* ro = option(a.recommended);
    a.recommendedInstalled = ro && ro->installed;
    if (const CatalogModel* cur = currentModel.empty() ? nullptr : catalog.Find(currentModel)) {
        a.upgradeSuggested = cur->tier < a.recommended->tier;
        if (!a.upgradeSuggested) a.notes.push_back("The model in use (" + cur->name + ") is already a good match.");
    }
    if (a.recommendedInstalled)
        a.notes.push_back("It is already installed.");
    else
        a.notes.push_back("Download it (" + FormatSize(a.recommended->SizeBytes()) +
                          "): downloads resume if interrupted and are checked against the published SHA-256.");
    if (ro && ro->fit == Fit::PartialOffload)
        a.notes.push_back("It does not fit in graphics memory entirely: part of it runs on the CPU, which is slower.");
    if (ro && ro->fit == Fit::CpuTight) a.notes.push_back("It needs most of the RAM: close other programs first.");
    return a;
}

}  // namespace modelhub
