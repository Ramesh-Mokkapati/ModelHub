#include "ServerPool.h"

#include <deque>
#include <regex>
#include <set>

#include "HttpClient.h"
#include "Text.h"

namespace modelhub {

namespace fs = std::filesystem;

ServerPool::ServerPool(ServerPoolOptions opt, HardwareInfo hw, LogFn log)
    : opt_(std::move(opt)), hw_(std::move(hw)), log_(std::move(log)) {
    reaper_ = std::jthread([this](std::stop_token stop) {
        std::mutex m;
        std::unique_lock lk(m);
        while (!stop.stop_requested()) {
            reaperCv_.wait_for(lk, stop, std::chrono::seconds(10), [] { return false; });
            if (!stop.stop_requested()) ReapIdle();
        }
    });
}

ServerPool::~ServerPool() {
    reaper_.request_stop();
    if (reaper_.joinable()) reaper_.join();
    StopAll();
}

ServerPool::Lease::~Lease() {
    if (pool_) pool_->Release(key_);
}

void ServerPool::Release(const std::string& key) {
    std::lock_guard lk(mu_);
    if (auto it = servers_.find(key); it != servers_.end()) {
        it->second.active = std::max(0, it->second.active - 1);
        it->second.lastUsed = std::chrono::steady_clock::now();
    }
}

void ServerPool::StopLocked(const std::string& key) {
    auto it = servers_.find(key);
    if (it == servers_.end()) return;
    if (log_) log_("stopping llama-server for " + it->second.modelId + " (port " + std::to_string(it->second.port) + ")");
    servers_.erase(it);  // ManagedProcess kills the process tree
}

void ServerPool::ReapIdle() {
    std::lock_guard lk(mu_);
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> stop;
    for (auto& [key, s] : servers_)
        if (s.active == 0 && (now - s.lastUsed > opt_.idleTimeout || !s.proc || !s.proc->IsRunning())) stop.push_back(key);
    for (const auto& k : stop) StopLocked(k);
}

void ServerPool::Stop(const std::string& modelId) {
    std::lock_guard lk(mu_);
    std::vector<std::string> stop;
    for (auto& [key, s] : servers_)
        if (s.modelId == modelId && s.active == 0) stop.push_back(key);
    for (const auto& k : stop) StopLocked(k);
}

void ServerPool::StopAll() {
    std::lock_guard lk(mu_);
    servers_.clear();
}

std::vector<ServerPool::Info> ServerPool::Running() {
    std::lock_guard lk(mu_);
    std::vector<Info> v;
    auto now = std::chrono::steady_clock::now();
    for (const auto& [_, s] : servers_) {
        if (!s.proc || !s.proc->IsRunning()) continue;
        v.push_back({s.modelId, s.port, s.gpuLayers, s.contextTokens, s.parallel, s.active, s.proc->Pid(), s.memoryMB,
                     std::chrono::duration_cast<std::chrono::seconds>(now - s.lastUsed).count()});
    }
    return v;
}

std::string ServerPool::ChooseDevice(const std::vector<std::string>& lines, const std::string& gpuName) {
    if (gpuName.empty() || gpuName.find("integrated") != std::string::npos) return {};
    std::string want = ToLower(gpuName);
    static const std::regex device(R"(^\s*([A-Za-z]+[0-9]+)\s*:\s*(.+?)\s*\()");
    for (const auto& l : lines) {
        std::smatch m;
        if (!std::regex_search(l, m, device)) continue;
        std::string name = ToLower(m[2].str());
        if (name.find(want) != std::string::npos || want.find(name) != std::string::npos) return m[1].str();
    }
    return {};
}

std::vector<std::wstring> ServerPool::BuildArgs(const fs::path& gguf, int port, int parallel, int contextTokens,
                                                int gpuLayers, const std::string& alias, bool embeddings,
                                                const std::string& device) {
    parallel = std::max(1, parallel);
    std::vector<std::wstring> a = {L"--model",        gguf.wstring(),
                                   L"--host",         L"127.0.0.1",
                                   L"--port",         std::to_wstring(port),
                                   L"--alias",        Widen(alias),
                                   L"--parallel",     std::to_wstring(parallel),
                                   L"--ctx-size",     std::to_wstring(contextTokens * parallel),
                                   L"--n-gpu-layers", std::to_wstring(gpuLayers),
                                   L"--no-webui"};
    a.push_back(embeddings ? L"--embeddings" : L"--jinja");  // chat templates with tool calling
    if (!device.empty()) {
        a.push_back(L"--device");
        a.push_back(Widen(device));
    }
    return a;
}

ServerPool::Lease ServerPool::Acquire(const CatalogModel& m, const fs::path& gguf, int contextTokens, int parallel,
                                      std::stop_token stop, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return Lease();
    };
    contextTokens = contextTokens > 0 ? std::clamp(contextTokens, 512, std::max(512, m.contextK * 1024)) : 8192;
    parallel = parallel > 0 ? parallel : opt_.defaultParallel;
    // Reuse any running server of this model whose context is large enough.
    auto findReady = [&]() -> Lease {
        std::lock_guard lk(mu_);
        for (auto& [key, s] : servers_)
            if (s.modelId == m.id && s.contextTokens >= contextTokens && s.proc && s.proc->IsRunning() && !s.url.empty()) {
                ++s.active;
                s.lastUsed = std::chrono::steady_clock::now();
                return Lease(this, key, s.url);
            }
        return Lease();
    };
    if (Lease l = findReady()) return l;

    std::lock_guard startLock(startMu_);
    if (Lease l = findReady()) return l;  // another request started it meanwhile
    std::error_code ec;
    if (!fs::is_regular_file(opt_.llamaServer, ec))
        return fail("llama-server not found at " + PathToUtf8(opt_.llamaServer));

    MemoryNeed need = NeedFor(m, contextTokens, parallel);
    // Free graphics memory: stop idle servers (least recently used first) until the model fits on the GPU.
    HardwareInfo budget = hw_;
    {
        std::lock_guard lk(mu_);
        auto usedGpu = [&] {
            uint64_t used = 0;
            for (const auto& [_, s] : servers_) used += s.gpuMB;
            return used;
        };
        while (hw_.vramMB > 0 && usedGpu() + need.totalMB > hw_.vramMB * 92 / 100) {
            std::string victim;
            auto oldest = std::chrono::steady_clock::time_point::max();
            for (const auto& [key, s] : servers_)
                if (s.active == 0 && s.gpuMB > 0 && s.lastUsed < oldest) {
                    oldest = s.lastUsed;
                    victim = key;
                }
            if (victim.empty()) break;  // the rest are busy: load with what is left
            if (log_) log_("making room for " + m.id);
            StopLocked(victim);
        }
        uint64_t used = usedGpu();
        budget.vramMB = hw_.vramMB > used ? hw_.vramMB - used : 0;
    }
    int ngl = m.embeddings && budget.vramMB == 0 ? 0 : GpuLayersFor(m, budget, contextTokens, parallel);
    if (!device_) {
        std::vector<std::string> lines;
        if (hw_.vramMB > 0) {
            ProcessSpec probe{opt_.llamaServer.wstring(), {L"--list-devices"}};
            probe.timeout = std::chrono::seconds(30);
            RunProcess(probe, [&](std::string_view l, bool) { lines.emplace_back(l); }, stop);
        }
        device_ = ChooseDevice(lines, hw_.gpuName);
        if (log_ && !device_->empty()) log_("llama.cpp device: " + *device_ + " (" + hw_.gpuName + ")");
    }

    std::set<int> used;
    {
        std::lock_guard lk(mu_);
        for (const auto& [_, s] : servers_) used.insert(s.port);
    }
    int port = opt_.basePort;
    for (; port < opt_.basePort + 200; ++port)
        if (!used.count(port) && HttpGet("http://127.0.0.1:" + std::to_string(port) + "/health", {}, 300).status == 0) break;

    auto tail = std::make_shared<std::pair<std::mutex, std::deque<std::string>>>();
    auto proc = std::make_unique<ManagedProcess>();
    ProcessSpec spec;
    spec.exe = opt_.llamaServer.wstring();
    spec.args = BuildArgs(gguf, port, parallel, contextTokens, ngl, m.id, m.embeddings, ngl > 0 ? *device_ : std::string());
    spec.args.insert(spec.args.end(), opt_.extraArgs.begin(), opt_.extraArgs.end());
    spec.workDir = opt_.llamaServer.parent_path();
    LogFn log = log_;
    std::string tag = "[llama:" + m.id + "] ";
    std::string err;
    if (!proc->Start(
            spec,
            [tail, log, tag](std::string_view line, bool) {
                std::string l(line);
                {
                    std::lock_guard lk2(tail->first);
                    tail->second.push_back(l);
                    if (tail->second.size() > 40) tail->second.pop_front();
                }
                std::string low = ToLower(l);
                if (log && (low.find("error") != std::string::npos || low.find("failed") != std::string::npos ||
                            low.find("offloaded") != std::string::npos))
                    log(tag + Trim(l));
            },
            &err))
        return fail("cannot start llama-server: " + err);
    if (log_)
        log_("starting llama-server for " + m.id + " on port " + std::to_string(port) + " (GPU layers: " +
             (ngl >= 999 ? std::string("all") : std::to_string(ngl)) + ", " + std::to_string(parallel) + " slots x " +
             std::to_string(contextTokens) + " tokens)");

    std::string base = "http://127.0.0.1:" + std::to_string(port);
    auto lastLines = [&] {
        std::lock_guard lk2(tail->first);
        std::string out;
        size_t from = tail->second.size() > 6 ? tail->second.size() - 6 : 0;
        for (size_t i = from; i < tail->second.size(); ++i) out += "\n  " + tail->second[i];
        return out;
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);  // large models load slowly
    while (HttpGet(base + "/health", {}, 2000).status != 200) {
        if (!proc->IsRunning()) return fail("llama-server exited while loading " + m.id + ":" + lastLines());
        if (stop.stop_requested()) return fail("cancelled");
        if (std::chrono::steady_clock::now() > deadline)
            return fail("llama-server did not become ready within 10 minutes" + lastLines());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (log_) log_("llama-server ready for " + m.id + " at " + base);

    Server s;
    s.modelId = m.id;
    s.proc = std::move(proc);
    s.port = port;
    s.gpuLayers = ngl;
    s.contextTokens = contextTokens;
    s.parallel = parallel;
    s.memoryMB = need.totalMB;
    s.gpuMB = ngl >= 999 ? need.totalMB : (m.layers > 0 ? need.totalMB * std::min(ngl, m.layers) / m.layers : 0);
    s.active = 1;
    s.lastUsed = std::chrono::steady_clock::now();
    s.url = base;
    std::string key = m.id + "|" + std::to_string(contextTokens);
    std::lock_guard lk(mu_);
    servers_[key] = std::move(s);
    return Lease(this, key, base);
}

}  // namespace modelhub
